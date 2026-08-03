#include "deckuipump.h"

#include "SDL_compat.h"

#include <chrono>
#include <utility>

namespace {
constexpr qint64 MinimumFrameIntervalMs = 16;
}

DeckUiPump::DeckUiPump(Clock clock)
    : m_Clock(clock ? std::move(clock) : monotonicMilliseconds)
{
}

DeckUiPump::Decision DeckUiPump::plan(bool open, bool wantsTextInput, bool dirty)
{
    Decision decision;
    const bool shouldUseTextInput = open && wantsTextInput;
    if (shouldUseTextInput != m_TextInputActive) {
        m_TextInputActive = shouldUseTextInput;
        decision.textInput = shouldUseTextInput
            ? TextInputAction::Start : TextInputAction::Stop;
    }

    if (open != m_WasOpen) {
        m_WasOpen = open;
        m_HasRendered = false;
    }
    if (!open || !dirty) {
        return decision;
    }

    const qint64 now = m_Clock();
    if (!m_HasRendered || now - m_LastRenderTime >= MinimumFrameIntervalMs) {
        m_HasRendered = true;
        m_LastRenderTime = now;
        decision.render = true;
    }
    return decision;
}

void DeckUiPump::applyTextInput(const Decision& decision)
{
    if (decision.textInput == TextInputAction::Start) {
        SDL_StartTextInput();
    }
    else if (decision.textInput == TextInputAction::Stop) {
        SDL_StopTextInput();
    }
}

qint64 DeckUiPump::monotonicMilliseconds()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
