#include "PagerNotifications.h"

#ifdef T_LORA_PAGER

#include "FSCommon.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "main.h"
#include "mesh/Throttle.h"
#include "modules/ExternalNotificationModule.h"

#ifdef HAS_I2S
#include "AudioThread.h"
#endif

#if HAS_SCREEN
#include "graphics/Screen.h"
#endif

// Lives in variant.cpp: AudioBoard.h can't be included alongside main.h (GpioPin clash).
void pagerSetCodecVolume(uint8_t percent);

namespace PagerNotifications
{
namespace
{

const char *const settingsFile = "/prefs/pager_notify.dat";
constexpr uint8_t FILE_MAGIC = 0x50;
constexpr uint8_t FILE_VERSION = 2;

struct __attribute__((packed)) SavedSettings {
    uint8_t magic;
    uint8_t version;
    uint8_t level;
    uint8_t lastLevel; // restored on unmute
    uint8_t vibrate;
    uint8_t scope;
    uint8_t repeat;
    uint8_t systemSounds;
};

const uint8_t volumeTable[MAX_LEVEL + 1] = {0, 20, 32, 45, 60, 75};

uint8_t level = DEFAULT_LEVEL;
uint8_t lastLevel = DEFAULT_LEVEL;
bool vibrateOn = true;
AlertScope scope = AlertScope::All;
Repeat repeatMode = Repeat::Once;
bool systemSounds = true;

uint8_t clampLevel(int l)
{
    return l < 0 ? 0 : (l > MAX_LEVEL ? MAX_LEVEL : l);
}

void applyCodecVolume(uint8_t lvl)
{
    pagerSetCodecVolume(codecVolumeFor(lvl));
}

bool loadSettings()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    auto f = FSCom.open(settingsFile, FILE_O_READ);
    if (!f)
        return false;
    SavedSettings s = {};
    size_t n = f.read(reinterpret_cast<uint8_t *>(&s), sizeof(s));
    f.close();
    if (n != sizeof(s) || s.magic != FILE_MAGIC || s.version != FILE_VERSION)
        return false;
    level = clampLevel(s.level);
    lastLevel = s.lastLevel >= 1 && s.lastLevel <= MAX_LEVEL ? s.lastLevel : DEFAULT_LEVEL;
    vibrateOn = s.vibrate != 0;
    scope = s.scope <= static_cast<uint8_t>(AlertScope::Nothing) ? static_cast<AlertScope>(s.scope) : AlertScope::All;
    repeatMode = s.repeat == static_cast<uint8_t>(Repeat::UntilRead) ? Repeat::UntilRead : Repeat::Once;
    systemSounds = s.systemSounds != 0;
    return true;
#else
    return false;
#endif
}

void saveSettings()
{
#ifdef FSCom
    SavedSettings s = {FILE_MAGIC,
                       FILE_VERSION,
                       level,
                       lastLevel,
                       static_cast<uint8_t>(vibrateOn),
                       static_cast<uint8_t>(scope),
                       static_cast<uint8_t>(repeatMode),
                       static_cast<uint8_t>(systemSounds)};
    FSCom.mkdir("/prefs");
    SafeFile f(settingsFile);
    f.write(reinterpret_cast<const uint8_t *>(&s), sizeof(s));
    if (!f.close())
        LOG_ERROR("Pager: could not save %s", settingsFile);
#endif
}

void applyToConfig()
{
    auto &en = moduleConfig.external_notification;
    const bool alerts = scope != AlertScope::Nothing;
    const bool sound = level > 0;

    en.enabled = true;
    en.use_i2s_as_buzzer = sound;
    en.alert_message_buzzer = alerts && sound;
    en.alert_bell_buzzer = alerts && sound;
    en.alert_message_vibra = alerts && vibrateOn;
    en.alert_bell_vibra = alerts && vibrateOn;
    // With Repeat: Once the alert lasts output_ms; a vibrate-only alert needs time to finish its pattern.
    if (en.output_ms == 0)
        en.output_ms = 1000;
    if (repeatMode == Repeat::Once)
        en.nag_timeout = 0;
    else if (en.nag_timeout == 0)
        en.nag_timeout = UNTIL_READ_MAX_SECS;

    // buzzer_mode also carries keypress feedback ("system sounds"); keep it where the enum allows.
    auto &mode = config.device.buzzer_mode;
    if (mode != meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY) {
        systemSounds = mode == meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED ||
                       mode == meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY;
    }
    switch (scope) {
    case AlertScope::All:
        mode = systemSounds ? meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED
                            : meshtastic_Config_DeviceConfig_BuzzerMode_NOTIFICATIONS_ONLY;
        break;
    case AlertScope::DMsOnly:
        mode = meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY;
        break;
    case AlertScope::Nothing:
        mode = systemSounds ? meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY
                            : meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
        break;
    }
}

void commit()
{
    applyToConfig();
    saveSettings();
    nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_MODULECONFIG);
    applyCodecVolume(level);
    LOG_INFO("Pager notifications: sound %u, vibrate %d, alert for %u, repeat %u", level, vibrateOn, (unsigned)scope,
             (unsigned)repeatMode);
}

#ifdef HAS_I2S
// Plays a short tone without blocking the UI, then restores the saved codec volume.
class SamplePlayer : public concurrency::OSThread
{
  public:
    SamplePlayer() : OSThread("PagerSample") { disable(); }

    void play(uint8_t lvl)
    {
        if (!audioThread || lvl == 0)
            return;
        if (externalNotificationModule && externalNotificationModule->nagging())
            return; // don't cut into a real alert
        static const char sample[] = "sample:d=8,o=6,b=200:c";
        audioThread->stop();
        pagerSetCodecVolume(codecVolumeFor(lvl));
        audioThread->beginRttl(sample, strlen(sample));
        startedAt = millis();
        playing = true;
        enabled = true;
        setIntervalFromNow(0);
    }

  protected:
    int32_t runOnce() override
    {
        // isPlaying() also pumps the tone generator
        if (playing && audioThread->isPlaying() && Throttle::isWithinTimespanMs(startedAt, 1000))
            return 5;
        if (playing) {
            audioThread->stop();
            playing = false;
            applyCodecVolume(level);
        }
        return disable();
    }

  private:
    bool playing = false;
    uint32_t startedAt = 0;
};

SamplePlayer *samplePlayer = nullptr;
#endif

void showLevelBanner()
{
#if HAS_SCREEN
    if (!screen)
        return;
    powerFSM.trigger(EVENT_INPUT); // wake the screen so the banner is visible
    static char msg[48];
    if (level == 0)
        snprintf(msg, sizeof(msg), "Muted");
    else
        snprintf(msg, sizeof(msg), "Volume %u", level);
    if (scope == AlertScope::Nothing)
        strncat(msg, "\nAlerts are off", sizeof(msg) - strlen(msg) - 1);
    screen->showSimpleBanner(msg, 1500);
#endif
}

} // namespace

uint8_t codecVolumeFor(uint8_t lvl)
{
    return volumeTable[clampLevel(lvl)];
}

void syncFromConfig()
{
    const auto &en = moduleConfig.external_notification;
    switch (config.device.buzzer_mode) {
    case meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED:
    case meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY:
        scope = AlertScope::Nothing;
        break;
    case meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY:
        scope = AlertScope::DMsOnly;
        break;
    default:
        scope = AlertScope::All;
        break;
    }
    if (!en.enabled)
        scope = AlertScope::Nothing;

    // With alerts off every flag is cleared, so keep the saved sound/vibrate choices.
    if (scope != AlertScope::Nothing) {
        const bool soundOn = en.use_i2s_as_buzzer && en.alert_message_buzzer;
        if (!soundOn)
            level = 0;
        else if (level == 0)
            level = lastLevel;
        vibrateOn = en.alert_message_vibra;
    }
    repeatMode = en.nag_timeout > 0 ? Repeat::UntilRead : Repeat::Once;
}

void begin()
{
#ifdef HAS_I2S
    if (!samplePlayer)
        samplePlayer = new SamplePlayer();
#endif
    if (loadSettings()) {
        syncFromConfig();
        applyCodecVolume(level);
    } else {
        // First run: keep the existing alert/vibrate/repeat choices, start sound at the default level.
        syncFromConfig();
        level = lastLevel = DEFAULT_LEVEL;
        commit();
    }
    LOG_INFO("Pager notifications: sound %u (codec %u%%), vibrate %d, alert for %u, repeat %u", level, codecVolumeFor(level),
             vibrateOn, (unsigned)scope, (unsigned)repeatMode);
}

uint8_t soundLevel()
{
    return level;
}

bool vibrate()
{
    return vibrateOn;
}

AlertScope alertScope()
{
    return scope;
}

Repeat repeat()
{
    return repeatMode;
}

void setSoundLevel(uint8_t lvl)
{
    level = clampLevel(lvl);
    if (level > 0)
        lastLevel = level;
    commit();
}

void setVibrate(bool on)
{
    vibrateOn = on;
    commit();
}

void setAlertScope(AlertScope s)
{
    scope = s;
    commit();
}

void setRepeat(Repeat r)
{
    repeatMode = r;
    commit();
}

void previewLevel(uint8_t lvl)
{
#ifdef HAS_I2S
    if (samplePlayer)
        samplePlayer->play(clampLevel(lvl));
#endif
}

void previewVibrate()
{
#ifdef HAS_DRV2605
    drv.setWaveform(0, 47); // buzz 100%
    drv.setWaveform(1, 0);
    drv.go();
#endif
}

void volumeUp()
{
    setSoundLevel(level < MAX_LEVEL ? level + 1 : MAX_LEVEL);
    previewLevel(level);
    showLevelBanner();
}

void volumeDown()
{
    setSoundLevel(level > 0 ? level - 1 : 0);
    previewLevel(level);
    showLevelBanner();
}

void toggleMute()
{
    if (externalNotificationModule && externalNotificationModule->nagging())
        externalNotificationModule->stopNow();
    setSoundLevel(level > 0 ? 0 : lastLevel);
    previewLevel(level);
    showLevelBanner();
}

} // namespace PagerNotifications

#endif
