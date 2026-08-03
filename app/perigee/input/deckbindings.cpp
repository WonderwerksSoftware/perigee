#include "deckbindings.h"

#include <QSettings>
#include <QStringList>

namespace {

constexpr auto KeyModifiersSetting = "deckKeyModifiers";
constexpr auto KeyScancodeSetting = "deckKeyScancode";
constexpr auto ControllerButtonsSetting = "deckControllerButtons";
constexpr auto LegacyDisconnectSetting = "legacyGamepadDisconnect";

quint32 buttonBit(int button)
{
    return button >= 0 && button < 32 ? quint32(1) << button : 0;
}

quint32 supportedControllerButtons()
{
    quint32 mask = 0;
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
        mask |= buttonBit(button);
    }
    return mask;
}

int populationCount(quint32 value)
{
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

bool isModifierScancode(int scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_RALT:
    case SDL_SCANCODE_RGUI:
        return true;
    default:
        return false;
    }
}

QString controllerButtonName(int button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return QStringLiteral("LB");
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return QStringLiteral("RB");
    case SDL_CONTROLLER_BUTTON_BACK: return QStringLiteral("Back");
    case SDL_CONTROLLER_BUTTON_START: return QStringLiteral("Start");
    case SDL_CONTROLLER_BUTTON_A: return QStringLiteral("A");
    case SDL_CONTROLLER_BUTTON_B: return QStringLiteral("B");
    case SDL_CONTROLLER_BUTTON_X: return QStringLiteral("X");
    case SDL_CONTROLLER_BUTTON_Y: return QStringLiteral("Y");
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return QStringLiteral("D-pad Up");
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return QStringLiteral("D-pad Down");
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return QStringLiteral("D-pad Left");
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return QStringLiteral("D-pad Right");
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return QStringLiteral("Left Stick");
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return QStringLiteral("Right Stick");
    case SDL_CONTROLLER_BUTTON_GUIDE: return QStringLiteral("Guide");
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_BUTTON_MISC1: return QStringLiteral("Misc");
    case SDL_CONTROLLER_BUTTON_PADDLE1: return QStringLiteral("Paddle 1");
    case SDL_CONTROLLER_BUTTON_PADDLE2: return QStringLiteral("Paddle 2");
    case SDL_CONTROLLER_BUTTON_PADDLE3: return QStringLiteral("Paddle 3");
    case SDL_CONTROLLER_BUTTON_PADDLE4: return QStringLiteral("Paddle 4");
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return QStringLiteral("Touchpad");
#endif
    default: return QString();
    }
}

}

DeckBindings::DeckBindings()
    : m_ControllerButtons(defaultControllerButtons())
{
}

DeckBindings::DeckBindings(int keyModifiers, int keyScancode,
                           quint32 controllerButtons,
                           bool legacyGamepadDisconnect)
    : DeckBindings()
{
    setKeyboardBinding(keyModifiers, keyScancode);
    setControllerBinding(controllerButtons);
    setLegacyGamepadDisconnect(legacyGamepadDisconnect);
}

DeckBindings DeckBindings::load(const QSettings& settings)
{
    return DeckBindings(
        settings.value(KeyModifiersSetting, DefaultKeyModifiers).toInt(),
        settings.value(KeyScancodeSetting, DefaultKeyScancode).toInt(),
        settings.value(ControllerButtonsSetting,
                       defaultControllerButtons()).toUInt(),
        settings.value(LegacyDisconnectSetting, false).toBool());
}

void DeckBindings::save(QSettings& settings) const
{
    settings.setValue(KeyModifiersSetting, m_KeyModifiers);
    settings.setValue(KeyScancodeSetting, m_KeyScancode);
    settings.setValue(ControllerButtonsSetting, m_ControllerButtons);
    settings.setValue(LegacyDisconnectSetting, m_LegacyGamepadDisconnect);
}

int DeckBindings::keyModifiers() const
{
    return m_KeyModifiers;
}

int DeckBindings::keyScancode() const
{
    return m_KeyScancode;
}

quint32 DeckBindings::controllerButtons() const
{
    return m_ControllerButtons;
}

bool DeckBindings::legacyGamepadDisconnect() const
{
    return m_LegacyGamepadDisconnect;
}

bool DeckBindings::setKeyboardBinding(int keyModifiers, int keyScancode)
{
    if (!isValidKeyboardBinding(keyModifiers, keyScancode)) {
        return false;
    }
    m_KeyModifiers = keyModifiers;
    m_KeyScancode = keyScancode;
    return true;
}

bool DeckBindings::setControllerBinding(quint32 controllerButtons)
{
    if (!isValidControllerBinding(controllerButtons)) {
        return false;
    }
    m_ControllerButtons = controllerButtons;
    return true;
}

void DeckBindings::setLegacyGamepadDisconnect(bool enabled)
{
    m_LegacyGamepadDisconnect = enabled;
}

void DeckBindings::resetToDefaults()
{
    m_KeyModifiers = DefaultKeyModifiers;
    m_KeyScancode = DefaultKeyScancode;
    m_ControllerButtons = defaultControllerButtons();
    m_LegacyGamepadDisconnect = false;
}

bool DeckBindings::isValidKeyboardBinding(int keyModifiers, int keyScancode)
{
    constexpr quint32 allowedModifiers =
        quint32(Qt::ShiftModifier) |
        quint32(Qt::ControlModifier) |
        quint32(Qt::AltModifier) |
        quint32(Qt::MetaModifier);
    const quint32 modifiers = quint32(keyModifiers);
    return modifiers != 0 &&
        (modifiers & ~allowedModifiers) == 0 &&
        keyScancode > SDL_SCANCODE_UNKNOWN &&
        keyScancode < SDL_NUM_SCANCODES &&
        !isModifierScancode(keyScancode);
}

bool DeckBindings::isValidControllerBinding(quint32 controllerButtons)
{
    return controllerButtons != 0 &&
        (controllerButtons & ~supportedControllerButtons()) == 0 &&
        populationCount(controllerButtons) >= 2 &&
        !conflictsWithStatsChord(controllerButtons);
}

bool DeckBindings::conflictsWithStatsChord(quint32 controllerButtons)
{
    return controllerButtons == statsControllerButtons();
}

QString DeckBindings::controllerConflictReason(quint32 controllerButtons)
{
    if (conflictsWithStatsChord(controllerButtons)) {
        return QStringLiteral(
            "This shortcut is reserved for Moonlight's performance statistics.");
    }
    if (!isValidControllerBinding(controllerButtons)) {
        return QStringLiteral(
            "Choose at least two supported controller buttons.");
    }
    return QString();
}

QString DeckBindings::formatKeyboardBinding(int keyModifiers, int keyScancode)
{
    if (!isValidKeyboardBinding(keyModifiers, keyScancode)) {
        return QString();
    }
    QStringList names;
    if (keyModifiers & Qt::ControlModifier) names.push_back(QStringLiteral("Ctrl"));
    if (keyModifiers & Qt::AltModifier) names.push_back(QStringLiteral("Alt"));
    if (keyModifiers & Qt::ShiftModifier) names.push_back(QStringLiteral("Shift"));
    if (keyModifiers & Qt::MetaModifier) names.push_back(QStringLiteral("Meta"));
    const char* keyName = SDL_GetScancodeName(SDL_Scancode(keyScancode));
    names.push_back(keyName != nullptr && keyName[0] != '\0'
                        ? QString::fromUtf8(keyName)
                        : QStringLiteral("Key %1").arg(keyScancode));
    return names.join(QLatin1Char('+'));
}

QString DeckBindings::formatControllerBinding(quint32 controllerButtons)
{
    if (!isValidControllerBinding(controllerButtons)) {
        return QString();
    }
    const int displayOrder[] = {
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_GUIDE,
#if SDL_VERSION_ATLEAST(2, 0, 14)
        SDL_CONTROLLER_BUTTON_MISC1,
        SDL_CONTROLLER_BUTTON_PADDLE1,
        SDL_CONTROLLER_BUTTON_PADDLE2,
        SDL_CONTROLLER_BUTTON_PADDLE3,
        SDL_CONTROLLER_BUTTON_PADDLE4,
        SDL_CONTROLLER_BUTTON_TOUCHPAD,
#endif
    };
    QStringList names;
    for (int button : displayOrder) {
        if (controllerButtons & buttonBit(button)) {
            names.push_back(controllerButtonName(button));
        }
    }
    return names.join(QLatin1Char('+'));
}

int DeckBindings::sdlScancodeForQtKey(int qtKey)
{
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return SDL_SCANCODE_A + (qtKey - Qt::Key_A);
    }
    if (qtKey >= Qt::Key_1 && qtKey <= Qt::Key_9) {
        return SDL_SCANCODE_1 + (qtKey - Qt::Key_1);
    }
    if (qtKey == Qt::Key_0) {
        return SDL_SCANCODE_0;
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12) {
        return SDL_SCANCODE_F1 + (qtKey - Qt::Key_F1);
    }
    if (qtKey >= Qt::Key_F13 && qtKey <= Qt::Key_F24) {
        return SDL_SCANCODE_F13 + (qtKey - Qt::Key_F13);
    }
    switch (qtKey) {
    case Qt::Key_Space: return SDL_SCANCODE_SPACE;
    case Qt::Key_Return: return SDL_SCANCODE_RETURN;
    case Qt::Key_Enter: return SDL_SCANCODE_KP_ENTER;
    case Qt::Key_Tab: return SDL_SCANCODE_TAB;
    case Qt::Key_Backspace: return SDL_SCANCODE_BACKSPACE;
    case Qt::Key_Delete: return SDL_SCANCODE_DELETE;
    case Qt::Key_Insert: return SDL_SCANCODE_INSERT;
    case Qt::Key_Home: return SDL_SCANCODE_HOME;
    case Qt::Key_End: return SDL_SCANCODE_END;
    case Qt::Key_PageUp: return SDL_SCANCODE_PAGEUP;
    case Qt::Key_PageDown: return SDL_SCANCODE_PAGEDOWN;
    case Qt::Key_Up: return SDL_SCANCODE_UP;
    case Qt::Key_Down: return SDL_SCANCODE_DOWN;
    case Qt::Key_Left: return SDL_SCANCODE_LEFT;
    case Qt::Key_Right: return SDL_SCANCODE_RIGHT;
    case Qt::Key_Minus: return SDL_SCANCODE_MINUS;
    case Qt::Key_Equal: return SDL_SCANCODE_EQUALS;
    case Qt::Key_BracketLeft: return SDL_SCANCODE_LEFTBRACKET;
    case Qt::Key_BracketRight: return SDL_SCANCODE_RIGHTBRACKET;
    case Qt::Key_Backslash: return SDL_SCANCODE_BACKSLASH;
    case Qt::Key_Semicolon: return SDL_SCANCODE_SEMICOLON;
    case Qt::Key_Apostrophe: return SDL_SCANCODE_APOSTROPHE;
    case Qt::Key_Comma: return SDL_SCANCODE_COMMA;
    case Qt::Key_Period: return SDL_SCANCODE_PERIOD;
    case Qt::Key_Slash: return SDL_SCANCODE_SLASH;
    case Qt::Key_QuoteLeft: return SDL_SCANCODE_GRAVE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

quint32 DeckBindings::defaultControllerButtons()
{
    return buttonBit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
        buttonBit(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) |
        buttonBit(SDL_CONTROLLER_BUTTON_BACK) |
        buttonBit(SDL_CONTROLLER_BUTTON_START);
}

quint32 DeckBindings::statsControllerButtons()
{
    return buttonBit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
        buttonBit(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) |
        buttonBit(SDL_CONTROLLER_BUTTON_BACK) |
        buttonBit(SDL_CONTROLLER_BUTTON_X);
}

void DeckControllerChordCapture::begin()
{
    m_Active = true;
    m_ButtonsDown = 0;
    m_CandidateButtons = 0;
}

void DeckControllerChordCapture::cancel()
{
    m_Active = false;
    m_ButtonsDown = 0;
    m_CandidateButtons = 0;
}

bool DeckControllerChordCapture::isActive() const
{
    return m_Active;
}

std::optional<quint32> DeckControllerChordCapture::handleButton(
        Uint8 button, bool pressed)
{
    if (!m_Active) {
        return std::nullopt;
    }
    const quint32 bit = buttonBit(button);
    if (bit == 0 || (bit & supportedControllerButtons()) == 0) {
        return std::nullopt;
    }
    if (pressed) {
        m_ButtonsDown |= bit;
        m_CandidateButtons |= bit;
        return std::nullopt;
    }
    m_ButtonsDown &= ~bit;
    if (m_CandidateButtons == 0 || m_ButtonsDown != 0) {
        return std::nullopt;
    }
    const quint32 completed = m_CandidateButtons;
    cancel();
    return completed;
}
