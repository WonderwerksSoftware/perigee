#pragma once

#include "deckbindings.h"
#include "SDL_compat.h"

#include <QHash>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <Qt>
#include <QVector>

#include <optional>

class DeckInputRouter final
{
public:
    DeckInputRouter();
    explicit DeckInputRouter(DeckBindings bindings,
                             bool swapFaceButtons = false);

    enum class Disposition {
        Passthrough,
        Consumed,
    };

    enum class Action {
        None,
        OpenFromKeyboard,
        OpenFromController,
        Close,
        ToggleStats,
        LegacyDisconnect,
        Key,
        TextInput,
        NavigateUp,
        NavigateDown,
        NavigateLeft,
        NavigateRight,
        Activate,
        Back,
        PreviousCategory,
        NextCategory,
        FocusSearch,
        PointerMove,
        PointerPress,
        PointerRelease,
        PointerWheel,
    };

    struct Result {
        Disposition disposition = Disposition::Passthrough;
        Action action = Action::None;
        SDL_JoystickID controllerId = -1;
        int key = Qt::Key_unknown;
        Qt::KeyboardModifiers keyModifiers = Qt::NoModifier;
        bool pressed = false;
        bool autoRepeat = false;
        QString text;
        QPointF position;
        Qt::MouseButton mouseButton = Qt::NoButton;
        Qt::MouseButtons mouseButtons = Qt::NoButton;
        QPoint wheelDelta;
        QVector<SDL_Event> replayEvents;
        bool replayToDeck = false;
        std::optional<SDL_Event> deferredEvent;
    };

    Result route(const SDL_Event& event);
    Result routeReplay(const SDL_Event& event) const;
    Result routeDeferred(const SDL_Event& event);

    bool isDeckOpen() const;
    SDL_JoystickID controllerOwner() const;
    void openForKeyboard();
    void closeDeck();
    void syncDeckOpen(bool open);
    void setPointerMapping(const QRect& streamViewport,
                           const QSize& deckLogicalSize);

private:
    Result routeKey(const SDL_KeyboardEvent& event);
    Result routeText(const SDL_TextInputEvent& event);
    Result routeControllerButton(const SDL_ControllerButtonEvent& event);
    Result routeControllerAxis(const SDL_ControllerAxisEvent& event);
    Result routeMouseMotion(const SDL_MouseMotionEvent& event);
    Result routeMouseButton(const SDL_MouseButtonEvent& event);
    Result routeMouseWheel(const SDL_MouseWheelEvent& event);
    Result routeKeyOrdinary(const SDL_KeyboardEvent& event) const;
    Result routeControllerButtonOrdinary(
        const SDL_ControllerButtonEvent& event) const;

    bool keyboardChordHeld(const SDL_KeyboardEvent& event) const;
    bool isKeyboardChordMember(SDL_Scancode scancode) const;
    quint32 controllerMask(SDL_JoystickID controller) const;
    quint32 statsControllerMask() const;
    quint8 viableControllerTargets(quint32 down,
                                   SDL_JoystickID controller) const;
    void beginReleaseTails();
    void setOpen(bool open, SDL_JoystickID owner);
    QPointF mapPointer(int x, int y, bool clamp, bool* accepted) const;
    static int qtKey(const SDL_Keysym& keysym);
    static Qt::KeyboardModifiers qtModifiers(SDL_Keymod modifiers);
    static Qt::MouseButton qtMouseButton(Uint8 button);

    static constexpr Sint16 AxisPressDeadzone = 16000;
    static constexpr Sint16 AxisReleaseDeadzone = 8000;

    bool m_Open = false;
    SDL_JoystickID m_ControllerOwner = -1;
    QSet<SDL_Scancode> m_KeysDown;
    QSet<SDL_Scancode> m_KeyReleaseTail;
    QHash<SDL_JoystickID, quint32> m_ButtonsDown;
    QHash<SDL_JoystickID, quint32> m_ButtonReleaseTail;
    QSet<SDL_JoystickID> m_ChordTriggered;
    bool m_KeyboardChordTriggered = false;
    bool m_HorizontalAxisEngaged = false;
    bool m_VerticalAxisEngaged = false;
    QRect m_StreamViewport;
    QSize m_DeckLogicalSize;
    QPointF m_LastPointerPosition;
    bool m_HasPointerPosition = false;
    Qt::MouseButtons m_LocalMouseButtons = Qt::NoButton;
    DeckBindings m_Bindings;
    bool m_SwapFaceButtons = false;

    struct Candidate {
        QVector<SDL_Event> events;
        bool replayToDeck = false;
    };
    Candidate m_KeyboardCandidate;
    QHash<SDL_JoystickID, Candidate> m_ControllerCandidates;
};
