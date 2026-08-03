#include "test_registry.h"

#include "perigee/input/deckinputrouter.h"

#include <QtTest>

namespace {

SDL_Event keyEvent(Uint32 type, SDL_Scancode scanCode, SDL_Keycode keyCode,
                   SDL_Keymod modifiers = KMOD_NONE, Uint8 repeat = 0)
{
    SDL_Event event {};
    event.type = type;
    event.key.type = type;
    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = repeat;
    event.key.keysym.scancode = scanCode;
    event.key.keysym.sym = keyCode;
    event.key.keysym.mod = modifiers;
    return event;
}

SDL_Event buttonEvent(Uint32 type, SDL_JoystickID controller,
                      SDL_GameControllerButton button)
{
    SDL_Event event {};
    event.type = type;
    event.cbutton.type = type;
    event.cbutton.which = controller;
    event.cbutton.button = button;
    event.cbutton.state = type == SDL_CONTROLLERBUTTONDOWN
        ? SDL_PRESSED : SDL_RELEASED;
    return event;
}

SDL_Event axisEvent(SDL_JoystickID controller, SDL_GameControllerAxis axis,
                    Sint16 value)
{
    SDL_Event event {};
    event.type = SDL_CONTROLLERAXISMOTION;
    event.caxis.type = SDL_CONTROLLERAXISMOTION;
    event.caxis.which = controller;
    event.caxis.axis = axis;
    event.caxis.value = value;
    return event;
}

SDL_Event mouseMotion(int x, int y)
{
    SDL_Event event {};
    event.type = SDL_MOUSEMOTION;
    event.motion.type = SDL_MOUSEMOTION;
    event.motion.x = x;
    event.motion.y = y;
    return event;
}

SDL_Event mouseButton(Uint32 type, Uint8 button, int x, int y)
{
    SDL_Event event {};
    event.type = type;
    event.button.type = type;
    event.button.state = type == SDL_MOUSEBUTTONDOWN
        ? SDL_PRESSED : SDL_RELEASED;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return event;
}

SDL_Event mouseWheel(int x, int y)
{
    SDL_Event event {};
    event.type = SDL_MOUSEWHEEL;
    event.wheel.type = SDL_MOUSEWHEEL;
    event.wheel.x = x;
    event.wheel.y = y;
    return event;
}

SDL_Event fingerEvent(Uint32 type)
{
    SDL_Event event {};
    event.type = type;
    event.tfinger.type = type;
    return event;
}

DeckInputRouter::Result routeChord(DeckInputRouter& router,
                                   SDL_JoystickID controller,
                                   SDL_GameControllerButton finalButton)
{
    router.route(buttonEvent(SDL_CONTROLLERBUTTONDOWN, controller,
                             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    router.route(buttonEvent(SDL_CONTROLLERBUTTONDOWN, controller,
                             SDL_CONTROLLER_BUTTON_BACK));
    router.route(buttonEvent(SDL_CONTROLLERBUTTONDOWN, controller,
                             SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    return router.route(buttonEvent(SDL_CONTROLLERBUTTONDOWN, controller,
                                    finalButton));
}

}

class DeckInputRouterTest : public QObject
{
    Q_OBJECT

private slots:
    void closedInputPassesThroughAndKeyboardShortcutOwnsReleaseTail();
    void controllerOwnerNavigatesAndOtherControllersAreSuppressed();
    void stickNavigationUsesPressAndReleaseDeadzones();
    void mouseMapsViewportAndRejectsOrClampsOutOfBoundsInput();
    void statsChordWinsWithoutOpeningDeck();
    void keyboardChordAcceptsAlternatePressOrderAndIgnoresRepeat();
    void controllerCloseConsumesEveryChordReleaseTail();
    void touchInputIsSuppressedOnlyWhileDeckIsOpen();
    void textInsertionComesOnlyFromSdlTextInput();
    void ownerRemovalAllowsSafeRecovery();
    void repeatedOpenCloseReturnsToCleanPassthrough();
};

void DeckInputRouterTest::closedInputPassesThroughAndKeyboardShortcutOwnsReleaseTail()
{
    DeckInputRouter router;

    QCOMPARE(router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_A, SDLK_a)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_A, SDLK_a)).disposition,
             DeckInputRouter::Disposition::Passthrough);

    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL, KMOD_CTRL));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LALT, SDLK_LALT,
                          SDL_Keymod(KMOD_CTRL | KMOD_ALT)));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT,
                          SDL_Keymod(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)));
    const auto open = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE,
        SDL_Keymod(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)));
    QCOMPARE(open.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(open.action, DeckInputRouter::Action::OpenFromKeyboard);
    QVERIFY(router.isDeckOpen());

    QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_SPACE, SDLK_SPACE)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT)).disposition,
             DeckInputRouter::Disposition::Consumed);

    const auto close = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE,
        SDL_Keymod(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)));
    QCOMPARE(close.action, DeckInputRouter::Action::Close);
    QVERIFY(!router.isDeckOpen());
    QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_SPACE, SDLK_SPACE)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_LCTRL, SDLK_LCTRL)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_LALT, SDLK_LALT)).disposition,
             DeckInputRouter::Disposition::Consumed);

    QCOMPARE(router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_C, SDLK_c)).disposition,
             DeckInputRouter::Disposition::Passthrough);
}

void DeckInputRouterTest::controllerOwnerNavigatesAndOtherControllersAreSuppressed()
{
    DeckInputRouter router;
    const auto open = routeChord(router, 41, SDL_CONTROLLER_BUTTON_START);
    QCOMPARE(open.action, DeckInputRouter::Action::OpenFromController);
    QCOMPARE(open.controllerId, SDL_JoystickID(41));
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(41));
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41,
                             SDL_CONTROLLER_BUTTON_START));
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41,
                             SDL_CONTROLLER_BUTTON_BACK));
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41,
                             SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41,
                             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));

    const auto ownerDown = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    QCOMPARE(ownerDown.action, DeckInputRouter::Action::NavigateDown);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_DPAD_DOWN)).action,
             DeckInputRouter::Action::None);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 41, SDL_CONTROLLER_BUTTON_DPAD_DOWN)).disposition,
             DeckInputRouter::Disposition::Consumed);

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_A)).action,
             DeckInputRouter::Action::Activate);
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41, SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_B)).action,
             DeckInputRouter::Action::Back);
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)).action,
             DeckInputRouter::Action::PreviousCategory);
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41,
                             SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)).action,
             DeckInputRouter::Action::NextCategory);
    router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 41,
                             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_Y)).action,
             DeckInputRouter::Action::FocusSearch);

    const auto nonOwner = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 73, SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(nonOwner.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(nonOwner.action, DeckInputRouter::Action::None);
    QCOMPARE(router.route(axisEvent(73, SDL_CONTROLLER_AXIS_LEFTX, 30000)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::stickNavigationUsesPressAndReleaseDeadzones()
{
    DeckInputRouter router;
    routeChord(router, 8, SDL_CONTROLLER_BUTTON_START);

    QCOMPARE(router.route(axisEvent(8, SDL_CONTROLLER_AXIS_LEFTY, 10000)).action,
             DeckInputRouter::Action::None);
    QCOMPARE(router.route(axisEvent(8, SDL_CONTROLLER_AXIS_LEFTY, 20000)).action,
             DeckInputRouter::Action::NavigateDown);
    QCOMPARE(router.route(axisEvent(8, SDL_CONTROLLER_AXIS_LEFTY, 26000)).action,
             DeckInputRouter::Action::None);
    QCOMPARE(router.route(axisEvent(8, SDL_CONTROLLER_AXIS_LEFTY, 7000)).action,
             DeckInputRouter::Action::None);
    QCOMPARE(router.route(axisEvent(8, SDL_CONTROLLER_AXIS_LEFTY, -20000)).action,
             DeckInputRouter::Action::NavigateUp);
    QCOMPARE(router.route(axisEvent(8, SDL_CONTROLLER_AXIS_LEFTX, 22000)).action,
             DeckInputRouter::Action::NavigateRight);
}

void DeckInputRouterTest::mouseMapsViewportAndRejectsOrClampsOutOfBoundsInput()
{
    DeckInputRouter router;
    router.setPointerMapping(QRect(100, 50, 800, 450), QSize(1600, 900));
    router.openForKeyboard();

    const auto center = router.route(mouseMotion(500, 275));
    QCOMPARE(center.action, DeckInputRouter::Action::PointerMove);
    QCOMPARE(center.position, QPointF(800.0, 450.0));

    const auto outside = router.route(mouseMotion(99, 275));
    QCOMPARE(outside.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(outside.action, DeckInputRouter::Action::None);
    QCOMPARE(router.route(mouseMotion(900, 275)).action,
             DeckInputRouter::Action::None);

    const auto press = router.route(mouseButton(
        SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 500, 275));
    QCOMPARE(press.action, DeckInputRouter::Action::PointerPress);
    QCOMPARE(press.mouseButton, Qt::LeftButton);

    const auto clampedRelease = router.route(mouseButton(
        SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 950, 500));
    QCOMPARE(clampedRelease.action, DeckInputRouter::Action::PointerRelease);
    QCOMPARE(clampedRelease.position, QPointF(1598.0, 898.0));

    const auto wheel = router.route(mouseWheel(2, -3));
    QCOMPARE(wheel.action, DeckInputRouter::Action::PointerWheel);
    QCOMPARE(wheel.wheelDelta, QPoint(240, -360));
}

void DeckInputRouterTest::statsChordWinsWithoutOpeningDeck()
{
    DeckInputRouter router;
    const auto stats = routeChord(router, 12, SDL_CONTROLLER_BUTTON_X);
    QCOMPARE(stats.action, DeckInputRouter::Action::ToggleStats);
    QVERIFY(!router.isDeckOpen());

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12, SDL_CONTROLLER_BUTTON_X)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12, SDL_CONTROLLER_BUTTON_BACK)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::keyboardChordAcceptsAlternatePressOrderAndIgnoresRepeat()
{
    DeckInputRouter router;

    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT,
                          KMOD_SHIFT));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL,
                          SDL_Keymod(KMOD_SHIFT | KMOD_CTRL)));
    const auto open = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_LALT, SDLK_LALT,
        SDL_Keymod(KMOD_SHIFT | KMOD_CTRL | KMOD_ALT)));
    QCOMPARE(open.action, DeckInputRouter::Action::OpenFromKeyboard);

    const auto repeatedSpace = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE,
        SDL_Keymod(KMOD_SHIFT | KMOD_CTRL | KMOD_ALT), 1));
    QCOMPARE(repeatedSpace.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(repeatedSpace.action, DeckInputRouter::Action::Key);
    QVERIFY(router.isDeckOpen());
}

void DeckInputRouterTest::controllerCloseConsumesEveryChordReleaseTail()
{
    DeckInputRouter router;
    routeChord(router, 19, SDL_CONTROLLER_BUTTON_START);
    for (const auto button : {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                              SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                              SDL_CONTROLLER_BUTTON_BACK,
                              SDL_CONTROLLER_BUTTON_START}) {
        router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 19, button));
    }

    const auto close = routeChord(router, 19, SDL_CONTROLLER_BUTTON_START);
    QCOMPARE(close.action, DeckInputRouter::Action::Close);
    QVERIFY(!router.isDeckOpen());
    for (const auto button : {SDL_CONTROLLER_BUTTON_START,
                              SDL_CONTROLLER_BUTTON_BACK,
                              SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                              SDL_CONTROLLER_BUTTON_LEFTSHOULDER}) {
        QCOMPARE(router.route(buttonEvent(SDL_CONTROLLERBUTTONUP, 19, button)).disposition,
                 DeckInputRouter::Disposition::Consumed);
    }
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 19, SDL_CONTROLLER_BUTTON_A)).disposition,
             DeckInputRouter::Disposition::Passthrough);
}

void DeckInputRouterTest::touchInputIsSuppressedOnlyWhileDeckIsOpen()
{
    DeckInputRouter router;
    QCOMPARE(router.route(fingerEvent(SDL_FINGERDOWN)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    router.openForKeyboard();
    QCOMPARE(router.route(fingerEvent(SDL_FINGERDOWN)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(fingerEvent(SDL_FINGERMOTION)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(fingerEvent(SDL_FINGERUP)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::textInsertionComesOnlyFromSdlTextInput()
{
    DeckInputRouter router;
    router.openForKeyboard();

    const auto key = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_E, SDLK_e));
    QCOMPARE(key.action, DeckInputRouter::Action::Key);
    QVERIFY(key.text.isEmpty());

    SDL_Event textEvent {};
    textEvent.type = SDL_TEXTINPUT;
    textEvent.text.type = SDL_TEXTINPUT;
    qstrncpy(textEvent.text.text, "é", sizeof(textEvent.text.text));
    const auto text = router.route(textEvent);
    QCOMPARE(text.action, DeckInputRouter::Action::TextInput);
    QCOMPARE(text.text, QString::fromUtf8("é"));
}

void DeckInputRouterTest::ownerRemovalAllowsSafeRecovery()
{
    DeckInputRouter router;
    routeChord(router, 31, SDL_CONTROLLER_BUTTON_START);

    SDL_Event removed {};
    removed.type = SDL_CONTROLLERDEVICEREMOVED;
    removed.cdevice.type = SDL_CONTROLLERDEVICEREMOVED;
    removed.cdevice.which = 31;
    QCOMPARE(router.route(removed).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(-1));

    const auto recovery = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 32, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(32));
    QCOMPARE(recovery.action, DeckInputRouter::Action::Back);
}

void DeckInputRouterTest::repeatedOpenCloseReturnsToCleanPassthrough()
{
    DeckInputRouter router;
    for (int cycle = 0; cycle < 100; ++cycle) {
        router.openForKeyboard();
        QCOMPARE(router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_A, SDLK_a)).disposition,
                 DeckInputRouter::Disposition::Consumed);
        router.closeDeck();
        QCOMPARE(router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_A, SDLK_a)).disposition,
                 DeckInputRouter::Disposition::Consumed);
        QCOMPARE(router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_Z, SDLK_z)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
        router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_Z, SDLK_z));
    }
}

REGISTER_PERIGEE_TEST(DeckInputRouterTest);

#include "test_deckinputrouter.moc"
