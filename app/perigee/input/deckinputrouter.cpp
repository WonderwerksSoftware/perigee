#include "deckinputrouter.h"

#include <QtGlobal>

#include <cmath>

namespace {

constexpr quint8 ConfiguredTarget = 0x1;
constexpr quint8 StatsTarget = 0x2;
constexpr quint8 LegacyDisconnectTarget = 0x4;

quint32 buttonBit(Uint8 button)
{
    return button < 32 ? quint32(1) << button : 0;
}

bool containsAny(const QSet<SDL_Scancode>& keys,
                 SDL_Scancode left, SDL_Scancode right)
{
    return keys.contains(left) || keys.contains(right);
}

DeckInputRouter::Result consumedResult()
{
    DeckInputRouter::Result result;
    result.disposition = DeckInputRouter::Disposition::Consumed;
    return result;
}

}

DeckInputRouter::DeckInputRouter() = default;

DeckInputRouter::DeckInputRouter(DeckBindings bindings, bool swapFaceButtons)
    : m_Bindings(std::move(bindings)),
      m_SwapFaceButtons(swapFaceButtons)
{
}

DeckInputRouter::Result DeckInputRouter::route(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return routeKey(event.key);
    case SDL_TEXTINPUT:
        return routeText(event.text);
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        return routeControllerButton(event.cbutton);
    case SDL_CONTROLLERAXISMOTION:
        return routeControllerAxis(event.caxis);
    case SDL_MOUSEMOTION:
        return routeMouseMotion(event.motion);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        return routeMouseButton(event.button);
    case SDL_MOUSEWHEEL:
        return routeMouseWheel(event.wheel);
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        return m_Open ? consumedResult() : Result {};
    case SDL_CONTROLLERDEVICEREMOVED:
        if (event.cdevice.which == m_ControllerOwner) {
            m_ControllerOwner = -1;
            m_HorizontalAxisEngaged = false;
            m_VerticalAxisEngaged = false;
        }
        m_ButtonsDown.remove(event.cdevice.which);
        m_ButtonReleaseTail.remove(event.cdevice.which);
        m_ChordTriggered.remove(event.cdevice.which);
        m_ControllerCandidates.remove(event.cdevice.which);
        return {};
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLERSENSORUPDATE:
    case SDL_CONTROLLERTOUCHPADDOWN:
    case SDL_CONTROLLERTOUCHPADUP:
    case SDL_CONTROLLERTOUCHPADMOTION:
        return m_Open ? consumedResult() : Result {};
#endif
    default:
        return {};
    }
}

DeckInputRouter::Result DeckInputRouter::routeReplay(const SDL_Event& event) const
{
    // Buffered replay is Deck-owned. Once an earlier local action closes Deck,
    // later buffered events are discarded rather than reclassified as host
    // input. Their physical releases are already covered by release tails.
    if (!m_Open && (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
                    event.type == SDL_CONTROLLERBUTTONDOWN ||
                    event.type == SDL_CONTROLLERBUTTONUP)) {
        return consumedResult();
    }
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return routeKeyOrdinary(event.key);
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        return routeControllerButtonOrdinary(event.cbutton);
    default:
        return {};
    }
}

DeckInputRouter::Result DeckInputRouter::routeDeferred(const SDL_Event& event)
{
    return route(event);
}

bool DeckInputRouter::isDeckOpen() const
{
    return m_Open;
}

SDL_JoystickID DeckInputRouter::controllerOwner() const
{
    return m_ControllerOwner;
}

void DeckInputRouter::openForKeyboard()
{
    setOpen(true, -1);
}

void DeckInputRouter::closeDeck()
{
    setOpen(false, -1);
}

void DeckInputRouter::syncDeckOpen(bool open)
{
    if (open != m_Open) {
        setOpen(open, open ? m_ControllerOwner : -1);
    }
}

void DeckInputRouter::setPointerMapping(const QRect& streamViewport,
                                        const QSize& deckLogicalSize)
{
    m_StreamViewport = streamViewport;
    m_DeckLogicalSize = deckLogicalSize;
}

DeckInputRouter::Result DeckInputRouter::routeKey(const SDL_KeyboardEvent& event)
{
    const bool pressed = event.state == SDL_PRESSED;
    const SDL_Scancode scanCode = event.keysym.scancode;
    const bool wasDown = m_KeysDown.contains(scanCode);
    if (pressed && event.repeat && m_KeyReleaseTail.contains(scanCode)) {
        return consumedResult();
    }
    if (!pressed && m_KeyReleaseTail.remove(scanCode)) {
        m_KeysDown.remove(scanCode);
        if (scanCode == SDL_Scancode(m_Bindings.keyScancode()) ||
                m_KeyReleaseTail.isEmpty()) {
            m_KeyboardChordTriggered = false;
        }
        return consumedResult();
    }

    if (!m_Open) {
        if (pressed) {
            m_KeysDown.insert(scanCode);
        }
        else {
            m_KeysDown.remove(scanCode);
        }

        if (pressed && !event.repeat && keyboardChordHeld(event) &&
                !m_KeyboardChordTriggered) {
            m_KeyboardChordTriggered = true;
            Result result = consumedResult();
            result.action = Action::OpenFromKeyboard;
            setOpen(true, -1);
            return result;
        }
        return {};
    }

    SDL_Event bufferedEvent {};
    bufferedEvent.type = event.type;
    bufferedEvent.key = event;
    const bool chordMember = isKeyboardChordMember(scanCode);
    if (!m_KeyboardCandidate.events.isEmpty() && !chordMember) {
        Candidate aborted = std::move(m_KeyboardCandidate);
        m_KeyboardCandidate = {};
        Result result = consumedResult();
        result.replayEvents = std::move(aborted.events);
        result.replayToDeck = aborted.replayToDeck;
        result.deferredEvent = bufferedEvent;
        return result;
    }
    if (pressed) {
        m_KeysDown.insert(scanCode);
    }
    else {
        m_KeysDown.remove(scanCode);
    }
    if (chordMember &&
            (!m_KeyboardCandidate.events.isEmpty() ||
             (pressed && !event.repeat))) {
        if (m_KeyboardCandidate.events.isEmpty()) {
            m_KeyboardCandidate.replayToDeck = m_Open;
        }
        if (!event.repeat && (!pressed || !wasDown)) {
            m_KeyboardCandidate.events.push_back(bufferedEvent);
        }
        else {
            return consumedResult();
        }
    }

    if (pressed && !event.repeat && keyboardChordHeld(event) &&
            !m_KeyboardChordTriggered) {
        m_KeyboardCandidate = {};
        m_KeyboardChordTriggered = true;
        Result result;
        result.disposition = Disposition::Consumed;
        if (m_Open) {
            result.action = Action::Close;
            setOpen(false, -1);
        }
        else {
            result.action = Action::OpenFromKeyboard;
            setOpen(true, -1);
        }
        return result;
    }

    if (!pressed && !m_KeyboardCandidate.events.isEmpty()) {
        Candidate aborted = std::move(m_KeyboardCandidate);
        m_KeyboardCandidate = {};
        Result result = consumedResult();
        result.replayEvents = std::move(aborted.events);
        result.replayToDeck = aborted.replayToDeck;
        return result;
    }

    if (!m_KeyboardCandidate.events.isEmpty()) {
        return consumedResult();
    }

    return routeKeyOrdinary(event);
}

DeckInputRouter::Result DeckInputRouter::routeKeyOrdinary(
        const SDL_KeyboardEvent& event) const
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    result.action = Action::Key;
    result.key = qtKey(event.keysym);
    result.keyModifiers = qtModifiers(SDL_Keymod(event.keysym.mod));
    result.pressed = event.state == SDL_PRESSED;
    result.autoRepeat = event.repeat != 0;
    return result;
}

DeckInputRouter::Result DeckInputRouter::routeText(const SDL_TextInputEvent& event)
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    result.action = Action::TextInput;
    result.text = QString::fromUtf8(event.text);
    return result;
}

DeckInputRouter::Result DeckInputRouter::routeControllerButton(
        const SDL_ControllerButtonEvent& event)
{
    const bool pressed = event.state == SDL_PRESSED;
    const quint32 bit = buttonBit(event.button);
    const quint32 oldDown = controllerMask(event.which);

    if (!pressed && (m_ButtonReleaseTail.value(event.which) & bit)) {
        quint32 tail = m_ButtonReleaseTail.value(event.which) & ~bit;
        if (tail == 0) {
            m_ButtonReleaseTail.remove(event.which);
        }
        else {
            m_ButtonReleaseTail[event.which] = tail;
        }
        const quint32 down = oldDown & ~bit;
        if (down == 0) {
            m_ButtonsDown.remove(event.which);
        }
        else {
            m_ButtonsDown[event.which] = down;
        }
        if (down == 0) {
            m_ChordTriggered.remove(event.which);
        }
        return consumedResult();
    }

    SDL_Event bufferedEvent {};
    bufferedEvent.type = event.type;
    bufferedEvent.cbutton = event;
    const bool wasDown = (oldDown & bit) != 0;
    const quint32 proposedDown = pressed ? oldDown | bit : oldDown & ~bit;
    auto candidateIt = m_ControllerCandidates.find(event.which);

    // The current event is deliberately not committed before replay. A local
    // replay can close Deck, which changes where the current event belongs.
    if (m_Open && candidateIt != m_ControllerCandidates.end() && pressed &&
            !wasDown &&
            viableControllerTargets(proposedDown, event.which) == 0) {
        Candidate aborted = std::move(candidateIt.value());
        m_ControllerCandidates.erase(candidateIt);
        Result result = consumedResult();
        result.replayEvents = std::move(aborted.events);
        result.replayToDeck = aborted.replayToDeck;
        result.deferredEvent = bufferedEvent;
        return result;
    }

    if (proposedDown == 0) {
        m_ButtonsDown.remove(event.which);
        m_ChordTriggered.remove(event.which);
    }
    else {
        m_ButtonsDown[event.which] = proposedDown;
    }

    // Closed-stream input is never buffered. Prefix holds, repeats, and
    // releases keep Moonlight's normal timing. On exact configured completion,
    // only the final event is consumed; overlay takeover neutralizes prefixes
    // that have already reached the host.
    if (!m_Open) {
        if (pressed && !wasDown &&
                !m_ChordTriggered.contains(event.which) &&
                !m_Bindings.legacyGamepadDisconnect() &&
                proposedDown == m_Bindings.controllerButtons()) {
            m_ChordTriggered.insert(event.which);
            Result result = consumedResult();
            result.action = Action::OpenFromController;
            result.controllerId = event.which;
            setOpen(true, event.which);
            return result;
        }
        return routeControllerButtonOrdinary(event);
    }

    if (candidateIt != m_ControllerCandidates.end() && !pressed) {
        candidateIt->events.push_back(bufferedEvent);
        Candidate aborted = std::move(candidateIt.value());
        m_ControllerCandidates.erase(candidateIt);
        Result result = consumedResult();
        result.replayEvents = std::move(aborted.events);
        result.replayToDeck = aborted.replayToDeck;
        return result;
    }

    const quint8 viableTargets =
        viableControllerTargets(proposedDown, event.which);
    if (candidateIt == m_ControllerCandidates.end() && pressed && !wasDown &&
            oldDown == 0 && viableTargets != 0) {
        Candidate candidate;
        candidate.replayToDeck = m_Open;
        candidateIt = m_ControllerCandidates.insert(event.which,
                                                     std::move(candidate));
        if (m_Open && m_ControllerOwner < 0) {
            m_ControllerOwner = event.which;
        }
    }
    if (candidateIt != m_ControllerCandidates.end() &&
            (!pressed || !wasDown)) {
        candidateIt->events.push_back(bufferedEvent);
    }

    if (pressed && !wasDown && candidateIt != m_ControllerCandidates.end() &&
            !m_ChordTriggered.contains(event.which)) {
        if ((viableTargets & ConfiguredTarget) != 0 &&
                proposedDown == m_Bindings.controllerButtons()) {
            m_ControllerCandidates.remove(event.which);
            m_ChordTriggered.insert(event.which);
            m_ButtonReleaseTail[event.which] |= proposedDown;
            Result result;
            result.disposition = Disposition::Consumed;
            result.controllerId = event.which;
            result.action = Action::Close;
            setOpen(false, -1);
            return result;
        }
        if ((viableTargets & StatsTarget) != 0 &&
                proposedDown == statsControllerMask()) {
            m_ControllerCandidates.erase(candidateIt);
            m_ChordTriggered.insert(event.which);
            Result result = consumedResult();
            result.controllerId = event.which;
            m_ButtonReleaseTail[event.which] |= proposedDown;
            result.action = Action::ToggleStats;
            return result;
        }
        if ((viableTargets & LegacyDisconnectTarget) != 0 &&
                proposedDown == DeckBindings::defaultControllerButtons()) {
            m_ControllerCandidates.remove(event.which);
            m_ChordTriggered.insert(event.which);
            m_ButtonReleaseTail[event.which] |= proposedDown;
            Result result = consumedResult();
            result.action = Action::LegacyDisconnect;
            result.controllerId = event.which;
            return result;
        }
    }

    candidateIt = m_ControllerCandidates.find(event.which);
    if (candidateIt != m_ControllerCandidates.end()) {
        return consumedResult();
    }

    if (m_ControllerOwner < 0 && pressed) {
        m_ControllerOwner = event.which;
    }
    if (wasDown) {
        return consumedResult();
    }
    return routeControllerButtonOrdinary(event);
}

quint8 DeckInputRouter::viableControllerTargets(
        quint32 down, SDL_JoystickID controller) const
{
    if (down == 0) {
        return 0;
    }

    quint8 targets = 0;
    if (!m_Open) {
        return 0;
    }

    const bool controllerCanOwnDeck =
        m_ControllerOwner < 0 || m_ControllerOwner == controller;
    if (!m_Bindings.legacyGamepadDisconnect() && controllerCanOwnDeck &&
            (down & ~m_Bindings.controllerButtons()) == 0) {
        targets |= ConfiguredTarget;
    }

    const quint32 stats = statsControllerMask();
    if ((down & ~stats) == 0) {
        targets |= StatsTarget;
    }

    const quint32 quit = DeckBindings::defaultControllerButtons();
    if (m_Bindings.legacyGamepadDisconnect() && (down & ~quit) == 0) {
        targets |= LegacyDisconnectTarget;
    }
    return targets;
}

DeckInputRouter::Result DeckInputRouter::routeControllerButtonOrdinary(
        const SDL_ControllerButtonEvent& event) const
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    result.controllerId = event.which;
    if (event.which != m_ControllerOwner || event.state != SDL_PRESSED) {
        return result;
    }
    Uint8 button = event.button;
    if (m_SwapFaceButtons) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            button = SDL_CONTROLLER_BUTTON_B;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            button = SDL_CONTROLLER_BUTTON_A;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            button = SDL_CONTROLLER_BUTTON_Y;
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            button = SDL_CONTROLLER_BUTTON_X;
            break;
        }
    }
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        result.action = Action::NavigateUp;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        result.action = Action::NavigateDown;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        result.action = Action::NavigateLeft;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        result.action = Action::NavigateRight;
        break;
    case SDL_CONTROLLER_BUTTON_A:
        result.action = Action::Activate;
        break;
    case SDL_CONTROLLER_BUTTON_B:
        result.action = Action::Back;
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        result.action = Action::PreviousCategory;
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        result.action = Action::NextCategory;
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        result.action = Action::FocusSearch;
        break;
    default:
        break;
    }
    return result;
}

DeckInputRouter::Result DeckInputRouter::routeControllerAxis(
        const SDL_ControllerAxisEvent& event)
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    result.controllerId = event.which;
    if (event.which != m_ControllerOwner) {
        return result;
    }

    if (event.axis == SDL_CONTROLLER_AXIS_LEFTX) {
        if (std::abs(int(event.value)) <= AxisReleaseDeadzone) {
            m_HorizontalAxisEngaged = false;
        }
        else if (!m_HorizontalAxisEngaged &&
                 std::abs(int(event.value)) >= AxisPressDeadzone) {
            m_HorizontalAxisEngaged = true;
            result.action = event.value < 0 ? Action::NavigateLeft
                                            : Action::NavigateRight;
        }
    }
    else if (event.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        if (std::abs(int(event.value)) <= AxisReleaseDeadzone) {
            m_VerticalAxisEngaged = false;
        }
        else if (!m_VerticalAxisEngaged &&
                 std::abs(int(event.value)) >= AxisPressDeadzone) {
            m_VerticalAxisEngaged = true;
            result.action = event.value < 0 ? Action::NavigateUp
                                            : Action::NavigateDown;
        }
    }
    return result;
}

DeckInputRouter::Result DeckInputRouter::routeMouseMotion(
        const SDL_MouseMotionEvent& event)
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    bool accepted = false;
    result.position = mapPointer(event.x, event.y, false, &accepted);
    if (accepted) {
        result.action = Action::PointerMove;
        result.mouseButtons = m_LocalMouseButtons;
        m_LastPointerPosition = result.position;
        m_HasPointerPosition = true;
    }
    return result;
}

DeckInputRouter::Result DeckInputRouter::routeMouseButton(
        const SDL_MouseButtonEvent& event)
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    result.mouseButton = qtMouseButton(event.button);
    if (result.mouseButton == Qt::NoButton) {
        return result;
    }
    const bool pressed = event.state == SDL_PRESSED;
    bool accepted = false;
    result.position = mapPointer(event.x, event.y, !pressed, &accepted);
    if (!accepted) {
        return result;
    }
    if (pressed) {
        m_LocalMouseButtons |= result.mouseButton;
        result.action = Action::PointerPress;
    }
    else {
        m_LocalMouseButtons &= ~result.mouseButton;
        result.action = Action::PointerRelease;
    }
    result.mouseButtons = m_LocalMouseButtons;
    m_LastPointerPosition = result.position;
    m_HasPointerPosition = true;
    return result;
}

DeckInputRouter::Result DeckInputRouter::routeMouseWheel(
        const SDL_MouseWheelEvent& event)
{
    if (!m_Open) {
        return {};
    }
    Result result;
    result.disposition = Disposition::Consumed;
    if (!m_HasPointerPosition) {
        return result;
    }
    result.action = Action::PointerWheel;
    result.position = m_LastPointerPosition;
    result.mouseButtons = m_LocalMouseButtons;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    const int x = event.preciseX != 0.0f ? qRound(event.preciseX * 120.0f)
                                         : event.x * 120;
    const int y = event.preciseY != 0.0f ? qRound(event.preciseY * 120.0f)
                                         : event.y * 120;
#else
    const int x = event.x * 120;
    const int y = event.y * 120;
#endif
    result.wheelDelta = QPoint(x, y);
    return result;
}

bool DeckInputRouter::keyboardChordHeld(const SDL_KeyboardEvent& event) const
{
    int modifiers = int(qtModifiers(SDL_Keymod(event.keysym.mod)));
    if (containsAny(m_KeysDown, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL)) {
        modifiers |= Qt::ControlModifier;
    }
    if (containsAny(m_KeysDown, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT)) {
        modifiers |= Qt::AltModifier;
    }
    if (containsAny(m_KeysDown, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT)) {
        modifiers |= Qt::ShiftModifier;
    }
    if (containsAny(m_KeysDown, SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI)) {
        modifiers |= Qt::MetaModifier;
    }
    const int requiredModifiers = m_Bindings.keyModifiers();
    return m_KeysDown.contains(SDL_Scancode(m_Bindings.keyScancode())) &&
        (modifiers & requiredModifiers) == requiredModifiers;
}

bool DeckInputRouter::isKeyboardChordMember(SDL_Scancode scancode) const
{
    if (scancode == SDL_Scancode(m_Bindings.keyScancode())) {
        return true;
    }
    const int modifiers = m_Bindings.keyModifiers();
    if ((modifiers & Qt::ControlModifier) &&
            (scancode == SDL_SCANCODE_LCTRL ||
             scancode == SDL_SCANCODE_RCTRL)) {
        return true;
    }
    if ((modifiers & Qt::AltModifier) &&
            (scancode == SDL_SCANCODE_LALT ||
             scancode == SDL_SCANCODE_RALT)) {
        return true;
    }
    if ((modifiers & Qt::ShiftModifier) &&
            (scancode == SDL_SCANCODE_LSHIFT ||
             scancode == SDL_SCANCODE_RSHIFT)) {
        return true;
    }
    return (modifiers & Qt::MetaModifier) &&
        (scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI);
}

quint32 DeckInputRouter::controllerMask(SDL_JoystickID controller) const
{
    return m_ButtonsDown.value(controller);
}

quint32 DeckInputRouter::statsControllerMask() const
{
    quint32 mask = DeckBindings::statsControllerButtons();
    if (m_SwapFaceButtons) {
        mask &= ~(quint32(1) << SDL_CONTROLLER_BUTTON_X);
        mask |= quint32(1) << SDL_CONTROLLER_BUTTON_Y;
    }
    return mask;
}

void DeckInputRouter::beginReleaseTails()
{
    m_KeyReleaseTail.unite(m_KeysDown);
    for (auto it = m_ButtonsDown.cbegin(); it != m_ButtonsDown.cend(); ++it) {
        m_ButtonReleaseTail[it.key()] |= it.value();
    }
}

void DeckInputRouter::setOpen(bool open, SDL_JoystickID owner)
{
    if (m_Open == open && (!open || m_ControllerOwner == owner)) {
        return;
    }
    beginReleaseTails();
    m_KeyboardCandidate = {};
    m_ControllerCandidates.clear();
    m_Open = open;
    m_ControllerOwner = open ? owner : -1;
    m_HorizontalAxisEngaged = false;
    m_VerticalAxisEngaged = false;
    m_LocalMouseButtons = Qt::NoButton;
    m_HasPointerPosition = false;
}

QPointF DeckInputRouter::mapPointer(int x, int y, bool clamp,
                                    bool* accepted) const
{
    *accepted = false;
    if (!m_StreamViewport.isValid() || m_StreamViewport.isEmpty() ||
            !m_DeckLogicalSize.isValid() || m_DeckLogicalSize.isEmpty()) {
        return {};
    }
    const int left = m_StreamViewport.x();
    const int top = m_StreamViewport.y();
    const int right = left + m_StreamViewport.width() - 1;
    const int bottom = top + m_StreamViewport.height() - 1;
    if (!clamp && (x < left || x > right || y < top || y > bottom)) {
        return {};
    }
    const int mappedX = qBound(left, x, right);
    const int mappedY = qBound(top, y, bottom);
    *accepted = true;
    return QPointF(
        qreal(mappedX - left) * m_DeckLogicalSize.width() /
            m_StreamViewport.width(),
        qreal(mappedY - top) * m_DeckLogicalSize.height() /
            m_StreamViewport.height());
}

int DeckInputRouter::qtKey(const SDL_Keysym& keysym)
{
    switch (keysym.sym) {
    case SDLK_UP: return Qt::Key_Up;
    case SDLK_DOWN: return Qt::Key_Down;
    case SDLK_LEFT: return Qt::Key_Left;
    case SDLK_RIGHT: return Qt::Key_Right;
    case SDLK_RETURN: return Qt::Key_Return;
    case SDLK_KP_ENTER: return Qt::Key_Enter;
    case SDLK_ESCAPE: return Qt::Key_Escape;
    case SDLK_BACKSPACE: return Qt::Key_Backspace;
    case SDLK_TAB: return Qt::Key_Tab;
    case SDLK_SPACE: return Qt::Key_Space;
    case SDLK_DELETE: return Qt::Key_Delete;
    case SDLK_HOME: return Qt::Key_Home;
    case SDLK_END: return Qt::Key_End;
    case SDLK_PAGEUP: return Qt::Key_PageUp;
    case SDLK_PAGEDOWN: return Qt::Key_PageDown;
    default:
        if (keysym.sym >= SDLK_a && keysym.sym <= SDLK_z) {
            return Qt::Key_A + (keysym.sym - SDLK_a);
        }
        if (keysym.sym >= SDLK_0 && keysym.sym <= SDLK_9) {
            return Qt::Key_0 + (keysym.sym - SDLK_0);
        }
        return keysym.sym >= 0x20 && keysym.sym <= 0x7e
            ? int(keysym.sym) : int(Qt::Key_unknown);
    }
}

Qt::KeyboardModifiers DeckInputRouter::qtModifiers(SDL_Keymod modifiers)
{
    Qt::KeyboardModifiers result = Qt::NoModifier;
    if (modifiers & KMOD_SHIFT) result |= Qt::ShiftModifier;
    if (modifiers & KMOD_CTRL) result |= Qt::ControlModifier;
    if (modifiers & KMOD_ALT) result |= Qt::AltModifier;
    if (modifiers & KMOD_GUI) result |= Qt::MetaModifier;
    return result;
}

Qt::MouseButton DeckInputRouter::qtMouseButton(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return Qt::LeftButton;
    case SDL_BUTTON_RIGHT: return Qt::RightButton;
    case SDL_BUTTON_MIDDLE: return Qt::MiddleButton;
    case SDL_BUTTON_X1: return Qt::BackButton;
    case SDL_BUTTON_X2: return Qt::ForwardButton;
    default: return Qt::NoButton;
    }
}
