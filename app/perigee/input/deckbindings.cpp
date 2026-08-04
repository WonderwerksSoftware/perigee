#include "deckbindings.h"

#include <QSettings>
#include <QStringList>

#ifdef Q_OS_LINUX
#include <linux/input-event-codes.h>
#endif

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

bool masksHavePrefixRelationship(quint32 left, quint32 right)
{
    return (left & right) == left || (left & right) == right;
}

quint32 statsControllerButtonsWithPhysicalY()
{
    return buttonBit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
        buttonBit(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) |
        buttonBit(SDL_CONTROLLER_BUTTON_BACK) |
        buttonBit(SDL_CONTROLLER_BUTTON_Y);
}

bool conflictsWithLegacyQuitOrdering(quint32 buttons)
{
    const quint32 quit = DeckBindings::defaultControllerButtons();
    return buttons != quit && (buttons & quit) == quit;
}

bool hasOppositeDpadDirections(quint32 buttons)
{
    const quint32 vertical =
        buttonBit(SDL_CONTROLLER_BUTTON_DPAD_UP) |
        buttonBit(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    const quint32 horizontal =
        buttonBit(SDL_CONTROLLER_BUTTON_DPAD_LEFT) |
        buttonBit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    return (buttons & vertical) == vertical ||
        (buttons & horizontal) == horizontal;
}

#ifdef Q_OS_LINUX
int sdlScancodeForLinuxEvdev(quint32 code)
{
    struct Mapping {
        quint32 evdev;
        SDL_Scancode sdl;
    };
    static const Mapping mappings[] = {
        {KEY_ESC, SDL_SCANCODE_ESCAPE},
        {KEY_1, SDL_SCANCODE_1}, {KEY_2, SDL_SCANCODE_2},
        {KEY_3, SDL_SCANCODE_3}, {KEY_4, SDL_SCANCODE_4},
        {KEY_5, SDL_SCANCODE_5}, {KEY_6, SDL_SCANCODE_6},
        {KEY_7, SDL_SCANCODE_7}, {KEY_8, SDL_SCANCODE_8},
        {KEY_9, SDL_SCANCODE_9}, {KEY_0, SDL_SCANCODE_0},
        {KEY_MINUS, SDL_SCANCODE_MINUS}, {KEY_EQUAL, SDL_SCANCODE_EQUALS},
        {KEY_BACKSPACE, SDL_SCANCODE_BACKSPACE}, {KEY_TAB, SDL_SCANCODE_TAB},
        {KEY_Q, SDL_SCANCODE_Q}, {KEY_W, SDL_SCANCODE_W},
        {KEY_E, SDL_SCANCODE_E}, {KEY_R, SDL_SCANCODE_R},
        {KEY_T, SDL_SCANCODE_T}, {KEY_Y, SDL_SCANCODE_Y},
        {KEY_U, SDL_SCANCODE_U}, {KEY_I, SDL_SCANCODE_I},
        {KEY_O, SDL_SCANCODE_O}, {KEY_P, SDL_SCANCODE_P},
        {KEY_LEFTBRACE, SDL_SCANCODE_LEFTBRACKET},
        {KEY_RIGHTBRACE, SDL_SCANCODE_RIGHTBRACKET},
        {KEY_ENTER, SDL_SCANCODE_RETURN}, {KEY_LEFTCTRL, SDL_SCANCODE_LCTRL},
        {KEY_A, SDL_SCANCODE_A}, {KEY_S, SDL_SCANCODE_S},
        {KEY_D, SDL_SCANCODE_D}, {KEY_F, SDL_SCANCODE_F},
        {KEY_G, SDL_SCANCODE_G}, {KEY_H, SDL_SCANCODE_H},
        {KEY_J, SDL_SCANCODE_J}, {KEY_K, SDL_SCANCODE_K},
        {KEY_L, SDL_SCANCODE_L}, {KEY_SEMICOLON, SDL_SCANCODE_SEMICOLON},
        {KEY_APOSTROPHE, SDL_SCANCODE_APOSTROPHE},
        {KEY_GRAVE, SDL_SCANCODE_GRAVE}, {KEY_LEFTSHIFT, SDL_SCANCODE_LSHIFT},
        {KEY_BACKSLASH, SDL_SCANCODE_BACKSLASH},
        {KEY_Z, SDL_SCANCODE_Z}, {KEY_X, SDL_SCANCODE_X},
        {KEY_C, SDL_SCANCODE_C}, {KEY_V, SDL_SCANCODE_V},
        {KEY_B, SDL_SCANCODE_B}, {KEY_N, SDL_SCANCODE_N},
        {KEY_M, SDL_SCANCODE_M}, {KEY_COMMA, SDL_SCANCODE_COMMA},
        {KEY_DOT, SDL_SCANCODE_PERIOD}, {KEY_SLASH, SDL_SCANCODE_SLASH},
        {KEY_RIGHTSHIFT, SDL_SCANCODE_RSHIFT},
        {KEY_KPASTERISK, SDL_SCANCODE_KP_MULTIPLY},
        {KEY_LEFTALT, SDL_SCANCODE_LALT}, {KEY_SPACE, SDL_SCANCODE_SPACE},
        {KEY_CAPSLOCK, SDL_SCANCODE_CAPSLOCK},
        {KEY_F1, SDL_SCANCODE_F1}, {KEY_F2, SDL_SCANCODE_F2},
        {KEY_F3, SDL_SCANCODE_F3}, {KEY_F4, SDL_SCANCODE_F4},
        {KEY_F5, SDL_SCANCODE_F5}, {KEY_F6, SDL_SCANCODE_F6},
        {KEY_F7, SDL_SCANCODE_F7}, {KEY_F8, SDL_SCANCODE_F8},
        {KEY_F9, SDL_SCANCODE_F9}, {KEY_F10, SDL_SCANCODE_F10},
        {KEY_F11, SDL_SCANCODE_F11}, {KEY_F12, SDL_SCANCODE_F12},
        {KEY_NUMLOCK, SDL_SCANCODE_NUMLOCKCLEAR},
        {KEY_SCROLLLOCK, SDL_SCANCODE_SCROLLLOCK},
        {KEY_KP7, SDL_SCANCODE_KP_7}, {KEY_KP8, SDL_SCANCODE_KP_8},
        {KEY_KP9, SDL_SCANCODE_KP_9}, {KEY_KPMINUS, SDL_SCANCODE_KP_MINUS},
        {KEY_KP4, SDL_SCANCODE_KP_4}, {KEY_KP5, SDL_SCANCODE_KP_5},
        {KEY_KP6, SDL_SCANCODE_KP_6}, {KEY_KPPLUS, SDL_SCANCODE_KP_PLUS},
        {KEY_KP1, SDL_SCANCODE_KP_1}, {KEY_KP2, SDL_SCANCODE_KP_2},
        {KEY_KP3, SDL_SCANCODE_KP_3}, {KEY_KP0, SDL_SCANCODE_KP_0},
        {KEY_KPDOT, SDL_SCANCODE_KP_PERIOD},
        {KEY_102ND, SDL_SCANCODE_NONUSBACKSLASH},
        {KEY_KPENTER, SDL_SCANCODE_KP_ENTER},
        {KEY_RIGHTCTRL, SDL_SCANCODE_RCTRL},
        {KEY_KPSLASH, SDL_SCANCODE_KP_DIVIDE},
        {KEY_SYSRQ, SDL_SCANCODE_PRINTSCREEN},
        {KEY_RIGHTALT, SDL_SCANCODE_RALT},
        {KEY_HOME, SDL_SCANCODE_HOME}, {KEY_UP, SDL_SCANCODE_UP},
        {KEY_PAGEUP, SDL_SCANCODE_PAGEUP}, {KEY_LEFT, SDL_SCANCODE_LEFT},
        {KEY_RIGHT, SDL_SCANCODE_RIGHT}, {KEY_END, SDL_SCANCODE_END},
        {KEY_DOWN, SDL_SCANCODE_DOWN}, {KEY_PAGEDOWN, SDL_SCANCODE_PAGEDOWN},
        {KEY_INSERT, SDL_SCANCODE_INSERT}, {KEY_DELETE, SDL_SCANCODE_DELETE},
        {KEY_LEFTMETA, SDL_SCANCODE_LGUI}, {KEY_RIGHTMETA, SDL_SCANCODE_RGUI},
        {KEY_MENU, SDL_SCANCODE_APPLICATION},
        {KEY_F13, SDL_SCANCODE_F13}, {KEY_F14, SDL_SCANCODE_F14},
        {KEY_F15, SDL_SCANCODE_F15}, {KEY_F16, SDL_SCANCODE_F16},
        {KEY_F17, SDL_SCANCODE_F17}, {KEY_F18, SDL_SCANCODE_F18},
        {KEY_F19, SDL_SCANCODE_F19}, {KEY_F20, SDL_SCANCODE_F20},
        {KEY_F21, SDL_SCANCODE_F21}, {KEY_F22, SDL_SCANCODE_F22},
        {KEY_F23, SDL_SCANCODE_F23}, {KEY_F24, SDL_SCANCODE_F24},
    };
    for (const Mapping& mapping : mappings) {
        if (mapping.evdev == code) {
            return mapping.sdl;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}
#endif

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
        !conflictsWithStatsChord(controllerButtons) &&
        !conflictsWithLegacyQuitOrdering(controllerButtons) &&
        !hasOppositeDpadDirections(controllerButtons);
}

bool DeckBindings::conflictsWithStatsChord(quint32 controllerButtons)
{
    return masksHavePrefixRelationship(controllerButtons,
                                       statsControllerButtons()) ||
        masksHavePrefixRelationship(controllerButtons,
                                    statsControllerButtonsWithPhysicalY());
}

QString DeckBindings::controllerConflictReason(quint32 controllerButtons)
{
    if (conflictsWithStatsChord(controllerButtons)) {
        return QStringLiteral(
            "This shortcut overlaps the performance statistics shortcut.");
    }
    if (conflictsWithLegacyQuitOrdering(controllerButtons)) {
        return QStringLiteral(
            "This shortcut extends the direct disconnect shortcut.");
    }
    if (hasOppositeDpadDirections(controllerButtons)) {
        return QStringLiteral(
            "A shortcut cannot contain opposite D-pad directions.");
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

int DeckBindings::sdlScancodeForNativeKey(quint32 nativeScanCode,
                                          const QString& platformName)
{
#ifdef Q_OS_LINUX
    if (!platformName.startsWith(QStringLiteral("wayland")) ||
            nativeScanCode <= 8) {
        return SDL_SCANCODE_UNKNOWN;
    }
    return sdlScancodeForLinuxEvdev(nativeScanCode - 8);
#else
    Q_UNUSED(nativeScanCode);
    Q_UNUSED(platformName);
    return SDL_SCANCODE_UNKNOWN;
#endif
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
    m_PeakButtons = 0;
}

void DeckControllerChordCapture::cancel()
{
    m_Active = false;
    m_ButtonsDown = 0;
    m_PeakButtons = 0;
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
        if (populationCount(m_ButtonsDown) > populationCount(m_PeakButtons)) {
            m_PeakButtons = m_ButtonsDown;
        }
        return std::nullopt;
    }
    m_ButtonsDown &= ~bit;
    if (m_PeakButtons == 0 || m_ButtonsDown != 0) {
        return std::nullopt;
    }
    const quint32 completed = m_PeakButtons;
    cancel();
    return completed;
}
