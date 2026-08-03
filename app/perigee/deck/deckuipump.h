#pragma once

#include <QtGlobal>

#include <functional>

class DeckUiPump final
{
public:
    enum class TextInputAction { NoChange, Start, Stop };

    struct Decision {
        TextInputAction textInput = TextInputAction::NoChange;
        bool render = false;
    };

    using Clock = std::function<qint64()>;

    explicit DeckUiPump(Clock clock = {});
    Decision plan(bool open, bool wantsTextInput, bool dirty);
    void applyTextInput(const Decision& decision);

private:
    static qint64 monotonicMilliseconds();

    Clock m_Clock;
    bool m_WasOpen = false;
    bool m_TextInputActive = false;
    bool m_HasRendered = false;
    qint64 m_LastRenderTime = 0;
};
