#pragma once

#include "configuration.h"

#ifdef T_LORA_PAGER

#include <stdint.h>

// Pager notification settings, mapped onto the standard external_notification / buzzer_mode config so the
// phone apps stay consistent. Only the sound level has no config field; it lives in /prefs/pager_notify.dat.
namespace PagerNotifications
{

enum class AlertScope : uint8_t { All = 0, DMsOnly = 1, Nothing = 2 };
enum class Repeat : uint8_t { Once = 0, UntilRead = 1 };

constexpr uint8_t MAX_LEVEL = 5;
constexpr uint8_t DEFAULT_LEVEL = 3;
constexpr uint32_t UNTIL_READ_MAX_SECS = 600;

uint8_t codecVolumeFor(uint8_t level);

void begin();
void syncFromConfig();

uint8_t soundLevel(); // 0 = off
bool vibrate();
AlertScope alertScope();
Repeat repeat();

void setSoundLevel(uint8_t level);
void setVibrate(bool on);
void setAlertScope(AlertScope scope);
void setRepeat(Repeat repeat);

void previewLevel(uint8_t level);
void previewVibrate();

// Keyboard shortcuts: change, save, beep and show a banner.
void volumeUp();
void volumeDown();
void toggleMute();

} // namespace PagerNotifications

#endif
