#pragma once

#include "configuration.h"

#if defined(T_LORA_PAGER) && HAS_SCREEN

#include "input/InputBroker.h"

// Messages-screen reading mode: a short knob press toggles it, and the knob then scrolls the history
// instead of switching frames. The message menu moves to a long press.
namespace PagerReadingMode
{

constexpr uint32_t IDLE_EXIT_MS = 30 * 1000;
constexpr uint32_t LONG_PRESS_MS = 600;

bool active();
void exit();

// True when a long knob press has a meaning (messages screen with messages, no menu open).
bool wantsKnobLongPress();

// Called by Screen while the messages frame is showing; returns true if the event was consumed.
bool handleMessageFrameInput(const InputEvent *event);

} // namespace PagerReadingMode

#endif
