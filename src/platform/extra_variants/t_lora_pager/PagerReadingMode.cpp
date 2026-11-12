#include "PagerReadingMode.h"

#if defined(T_LORA_PAGER) && HAS_SCREEN

#include "MessageStore.h"
#include "concurrency/OSThread.h"
#include "graphics/Screen.h"
#include "graphics/draw/MenuHandler.h"
#include "graphics/draw/MessageRenderer.h"
#include "main.h"
#include "mesh/Throttle.h"
#include <string.h>

namespace PagerReadingMode
{
namespace
{

bool isActive = false;
uint32_t lastInputMs = 0;

class IdleWatcher : public concurrency::OSThread
{
  public:
    IdleWatcher() : OSThread("PagerReading") { disable(); }

    void start()
    {
        enabled = true;
        setIntervalFromNow(1000);
    }

  protected:
    int32_t runOnce() override
    {
        if (!isActive)
            return disable();
        // A new message resets the renderer's scroll state, which also ends reading mode.
        const bool stillReading = Throttle::isWithinTimespanMs(lastInputMs, IDLE_EXIT_MS) &&
                                  graphics::MessageRenderer::isManualScrolling() && screen && screen->isTextMessageFrameShowing();
        if (!stillReading) {
            exit();
            return disable();
        }
        return 1000;
    }
};

IdleWatcher *watcher = nullptr;

bool fromKnob(const InputEvent *event)
{
    return event->source && strcmp(event->source, "RotaryEncoder") == 0;
}

void enter()
{
    isActive = true;
    lastInputMs = millis();
    graphics::MessageRenderer::startManualScroll();
    if (!watcher)
        watcher = new IdleWatcher();
    watcher->start();
    LOG_INFO("Reading mode on");
}

} // namespace

bool active()
{
    return isActive;
}

void exit()
{
    if (!isActive)
        return;
    isActive = false;
    graphics::MessageRenderer::resetScrollState();
    LOG_INFO("Reading mode off");
}

bool wantsKnobLongPress()
{
    return screen && screen->isTextMessageFrameShowing() && !screen->isOverlayBannerShowing() &&
           !messageStore.getMessages().empty();
}

bool handleMessageFrameInput(const InputEvent *event)
{
    if (messageStore.getMessages().empty()) {
        exit();
        return false;
    }

    const input_broker_event ev = event->inputEvent;
    if (ev == INPUT_BROKER_SELECT_LONG) {
        exit();
        graphics::menuHandler::messageResponseMenu();
        return true;
    }

    if (!isActive) {
        if (ev == INPUT_BROKER_SELECT && fromKnob(event)) {
            enter();
            return true;
        }
        return false;
    }

    lastInputMs = millis();
    switch (ev) {
    case INPUT_BROKER_USER_PRESS: // knob clockwise: older messages
        graphics::MessageRenderer::scrollDown();
        return true;
    case INPUT_BROKER_ALT_PRESS: // knob counter-clockwise: newer messages
        graphics::MessageRenderer::scrollUp();
        return true;
    case INPUT_BROKER_SELECT:
        if (fromKnob(event)) {
            exit();
            return true;
        }
        break;
    case INPUT_BROKER_CANCEL:
        exit();
        return true;
    default:
        break;
    }
    // Any other input leaves reading mode and is handled as usual.
    exit();
    return false;
}

} // namespace PagerReadingMode

#endif
