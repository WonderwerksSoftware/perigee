#include "test_registry.h"

#include "perigee/input/deckbindings.h"
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
    void controllerNavigationHonorsFaceButtonSwap();
    void stickNavigationUsesPressAndReleaseDeadzones();
    void mouseMapsViewportAndRejectsOrClampsOutOfBoundsInput();
    void statsChordPassesThroughWhenClosedAndStaysLocalWhenOpen();
    void openStatsChordHonorsFaceSwapAndWinsOverDeckCandidate();
    void keyboardChordAcceptsAlternatePressOrderAndIgnoresRepeat();
    void controllerCloseConsumesEveryChordReleaseTail();
    void touchInputIsSuppressedOnlyWhileDeckIsOpen();
    void textInsertionComesOnlyFromSdlTextInput();
    void ownerRemovalAllowsSafeRecovery();
    void repeatedOpenCloseReturnsToCleanPassthrough();
    void configuredPhysicalChordsReplaceTheDefaults();
    void legacyModePassesOriginalControllerDisconnectThrough();
    void incompleteKeyboardChordCannotActivateDeckBeforeCompletion();
    void closedControllerPrefixPassesHostWhileOpenPrefixStaysLocal();
    void controllerCandidatesAreIsolatedPerPhysicalController();
    void openKeyboardCandidateCoalescesRepeatsBeforeReplay();
    void abortedOpenControllerCandidateReplaysOneLocalAction();
    void nonmemberAbortReplaysPrefixBeforeRoutingCurrentEvent();
    void controllerRemovalDiscardsOnlyItsPhysicalState();
    void reservedQuitAlwaysPassesThroughWhenDeckClosed();
    void closedStatsPassesThroughImmediatelyInEveryPressOrder();
    void externalVisibilitySyncClearsKeyboardCandidates();
    void externalVisibilitySyncClearsEveryControllerCandidate();
    void candidateAbortDefersControllerInputUntilAfterReplay();
    void candidateAbortDefersKeyboardInputUntilAfterReplay();
    void closedOrdinaryHoldsPassThroughImmediatelyWithoutReplay();
    void closedKeyboardRepeatsStayImmediateAndBounded();
    void openLegacyDisconnectChordWinsBeforeLocalNavigation();
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
             DeckInputRouter::Action::None);
    const auto leftShoulder = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONUP, 41, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    QCOMPARE(leftShoulder.replayEvents.size(), 2);
    QCOMPARE(router.routeReplay(leftShoulder.replayEvents.first()).action,
             DeckInputRouter::Action::PreviousCategory);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)).action,
             DeckInputRouter::Action::None);
    const auto rightShoulder = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONUP, 41, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    QCOMPARE(rightShoulder.replayEvents.size(), 2);
    QCOMPARE(router.routeReplay(rightShoulder.replayEvents.first()).action,
             DeckInputRouter::Action::NextCategory);
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

void DeckInputRouterTest::controllerNavigationHonorsFaceButtonSwap()
{
    DeckInputRouter router(DeckBindings(), true);
    router.openForKeyboard();

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 42,
                 SDL_CONTROLLER_BUTTON_B)).action,
             DeckInputRouter::Action::Activate);
    router.route(buttonEvent(
        SDL_CONTROLLERBUTTONUP, 42, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 42,
                 SDL_CONTROLLER_BUTTON_A)).action,
             DeckInputRouter::Action::Back);
    router.route(buttonEvent(
        SDL_CONTROLLERBUTTONUP, 42, SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 42,
                 SDL_CONTROLLER_BUTTON_X)).action,
             DeckInputRouter::Action::FocusSearch);
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

void DeckInputRouterTest::statsChordPassesThroughWhenClosedAndStaysLocalWhenOpen()
{
    DeckInputRouter router;
    const auto closedStats = routeChord(router, 12, SDL_CONTROLLER_BUTTON_X);
    QCOMPARE(closedStats.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(closedStats.action, DeckInputRouter::Action::None);
    QVERIFY(closedStats.replayEvents.isEmpty());
    QVERIFY(!router.isDeckOpen());
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12, SDL_CONTROLLER_BUTTON_X)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12, SDL_CONTROLLER_BUTTON_BACK)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12,
                 SDL_CONTROLLER_BUTTON_LEFTSHOULDER)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12,
                 SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)).disposition,
             DeckInputRouter::Disposition::Passthrough);

    router.openForKeyboard();
    const auto stats = routeChord(router, 12, SDL_CONTROLLER_BUTTON_X);
    QCOMPARE(stats.action, DeckInputRouter::Action::ToggleStats);
    QVERIFY(router.isDeckOpen());

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12, SDL_CONTROLLER_BUTTON_X)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 12, SDL_CONTROLLER_BUTTON_BACK)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::openStatsChordHonorsFaceSwapAndWinsOverDeckCandidate()
{
    for (bool swapFaceButtons : {false, true}) {
        const SDL_GameControllerButton faceButton =
            swapFaceButtons ? SDL_CONTROLLER_BUTTON_Y
                            : SDL_CONTROLLER_BUTTON_X;
        const QList<QList<SDL_GameControllerButton>> pressOrders {
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
             SDL_CONTROLLER_BUTTON_BACK,
             faceButton},
            {faceButton,
             SDL_CONTROLLER_BUTTON_BACK,
             SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        };
        for (const auto& order : pressOrders) {
            DeckInputRouter router(DeckBindings(), swapFaceButtons);
            router.openForKeyboard();
            DeckInputRouter::Result stats;
            for (int i = 0; i < order.size(); ++i) {
                const SDL_GameControllerButton button = order.at(i);
                const auto partial = router.route(buttonEvent(
                    SDL_CONTROLLERBUTTONDOWN, 52, button));
                QCOMPARE(partial.disposition,
                         DeckInputRouter::Disposition::Consumed);
                if (i + 1 == order.size()) {
                    stats = partial;
                }
                else {
                    QCOMPARE(partial.action, DeckInputRouter::Action::None);
                }
            }
            QCOMPARE(stats.action, DeckInputRouter::Action::ToggleStats);
            QVERIFY(stats.replayEvents.isEmpty());
            QVERIFY(router.isDeckOpen());
        }
    }
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
    QCOMPARE(repeatedSpace.action, DeckInputRouter::Action::None);
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

void DeckInputRouterTest::configuredPhysicalChordsReplaceTheDefaults()
{
    const quint32 controllerChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier | Qt::AltModifier),
        SDL_SCANCODE_F8,
        controllerChord,
        false));

    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL,
                          KMOD_CTRL));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LALT, SDLK_LALT,
                          SDL_Keymod(KMOD_CTRL | KMOD_ALT)));
    const auto oldDefault = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE,
        SDL_Keymod(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)));
    QCOMPARE(oldDefault.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(oldDefault.replayEvents.isEmpty());
    QVERIFY(!oldDefault.deferredEvent.has_value());
    QVERIFY(!router.isDeckOpen());
    router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_SPACE, SDLK_SPACE));

    const auto keyboardOpen = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_F8, SDLK_F8,
        SDL_Keymod(KMOD_CTRL | KMOD_ALT)));
    QCOMPARE(keyboardOpen.action, DeckInputRouter::Action::OpenFromKeyboard);
    QVERIFY(router.isDeckOpen());
    router.closeDeck();
    router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_F8, SDLK_F8));
    router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_LALT, SDLK_LALT));
    router.route(keyEvent(SDL_KEYUP, SDL_SCANCODE_LCTRL, SDLK_LCTRL));

    router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 84, SDL_CONTROLLER_BUTTON_A));
    const auto controllerOpen = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 84, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(controllerOpen.action,
             DeckInputRouter::Action::OpenFromController);
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(84));
}

void DeckInputRouterTest::legacyModePassesOriginalControllerDisconnectThrough()
{
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier),
        SDL_SCANCODE_SPACE,
        (quint32(1) << SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
            (quint32(1) << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) |
            (quint32(1) << SDL_CONTROLLER_BUTTON_BACK) |
            (quint32(1) << SDL_CONTROLLER_BUTTON_START),
        true));

    const auto legacyChord = routeChord(
        router, 91, SDL_CONTROLLER_BUTTON_START);
    QCOMPARE(legacyChord.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(legacyChord.action, DeckInputRouter::Action::None);
    QVERIFY(!router.isDeckOpen());

    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL,
                          KMOD_CTRL));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LALT, SDLK_LALT,
                          SDL_Keymod(KMOD_CTRL | KMOD_ALT)));
    router.route(keyEvent(SDL_KEYDOWN, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT,
                          SDL_Keymod(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)));
    const auto keyboardOpen = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE,
        SDL_Keymod(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)));
    QCOMPARE(keyboardOpen.action, DeckInputRouter::Action::OpenFromKeyboard);
    QVERIFY(router.isDeckOpen());
}

void DeckInputRouterTest::incompleteKeyboardChordCannotActivateDeckBeforeCompletion()
{
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier),
        SDL_SCANCODE_RETURN,
        DeckBindings::defaultControllerButtons(),
        false));
    router.openForKeyboard();

    const auto enterPrefix = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN));
    QCOMPARE(enterPrefix.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(enterPrefix.action, DeckInputRouter::Action::None);
    const auto completed = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL, KMOD_CTRL));
    QCOMPARE(completed.action, DeckInputRouter::Action::Close);
    QVERIFY(!router.isDeckOpen());
    QCOMPARE(router.route(keyEvent(
                 SDL_KEYUP, SDL_SCANCODE_RETURN, SDLK_RETURN)).disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(router.route(keyEvent(
                 SDL_KEYUP, SDL_SCANCODE_LCTRL, SDLK_LCTRL)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::closedControllerPrefixPassesHostWhileOpenPrefixStaysLocal()
{
    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));

    const auto prefix = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 71, SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(prefix.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(prefix.action, DeckInputRouter::Action::None);
    const auto completed = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 71, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(completed.action, DeckInputRouter::Action::OpenFromController);
    QVERIFY(router.isDeckOpen());
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 71, SDL_CONTROLLER_BUTTON_A)).action,
             DeckInputRouter::Action::None);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONUP, 71, SDL_CONTROLLER_BUTTON_B)).action,
             DeckInputRouter::Action::None);

    const auto openPrefix = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 71, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(openPrefix.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(openPrefix.action, DeckInputRouter::Action::None);
    const auto close = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 71, SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(close.action, DeckInputRouter::Action::Close);
}

void DeckInputRouterTest::controllerCandidatesAreIsolatedPerPhysicalController()
{
    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_A)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 18, SDL_CONTROLLER_BUTTON_A)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_B)).action,
             DeckInputRouter::Action::OpenFromController);
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(17));
    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 18, SDL_CONTROLLER_BUTTON_B)).action,
             DeckInputRouter::Action::None);
}

void DeckInputRouterTest::openKeyboardCandidateCoalescesRepeatsBeforeReplay()
{
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier),
        SDL_SCANCODE_RETURN,
        DeckBindings::defaultControllerButtons(),
        false));
    router.openForKeyboard();

    QCOMPARE(router.route(keyEvent(
                 SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN)).disposition,
             DeckInputRouter::Disposition::Consumed);
    for (int duplicate = 0; duplicate < 256; ++duplicate) {
        QCOMPARE(router.route(keyEvent(
                     SDL_KEYDOWN, SDL_SCANCODE_RETURN,
                     SDLK_RETURN, KMOD_NONE, 0)).disposition,
                 DeckInputRouter::Disposition::Consumed);
    }
    QCOMPARE(router.route(keyEvent(
                 SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN,
                 KMOD_NONE, 1)).disposition,
             DeckInputRouter::Disposition::Consumed);
    const auto aborted = router.route(keyEvent(
        SDL_KEYUP, SDL_SCANCODE_RETURN, SDLK_RETURN));

    QCOMPARE(aborted.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(aborted.replayEvents.size(), 2);
    QCOMPARE(aborted.replayEvents.at(0).type, Uint32(SDL_KEYDOWN));
    QCOMPARE(aborted.replayEvents.at(0).key.repeat, Uint8(0));
    QCOMPARE(aborted.replayEvents.at(1).type, Uint32(SDL_KEYUP));
}

void DeckInputRouterTest::abortedOpenControllerCandidateReplaysOneLocalAction()
{
    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));
    router.openForKeyboard();

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_A)).action,
             DeckInputRouter::Action::None);
    const auto aborted = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONUP, 17, SDL_CONTROLLER_BUTTON_A));

    QCOMPARE(aborted.replayEvents.size(), 2);
    QCOMPARE(router.routeReplay(aborted.replayEvents.at(0)).action,
             DeckInputRouter::Action::Activate);
    QCOMPARE(router.routeReplay(aborted.replayEvents.at(1)).action,
             DeckInputRouter::Action::None);
}

void DeckInputRouterTest::nonmemberAbortReplaysPrefixBeforeRoutingCurrentEvent()
{
    DeckInputRouter keyboardRouter(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_RETURN,
        DeckBindings::defaultControllerButtons(), false));
    keyboardRouter.openForKeyboard();
    QCOMPARE(keyboardRouter.route(keyEvent(
                 SDL_KEYDOWN, SDL_SCANCODE_LCTRL, SDLK_LCTRL, KMOD_CTRL)).action,
             DeckInputRouter::Action::None);
    const auto keyboardAbort = keyboardRouter.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_C, SDLK_c, KMOD_CTRL));
    QCOMPARE(keyboardAbort.disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(keyboardAbort.replayEvents.size(), 1);
    QVERIFY(keyboardAbort.deferredEvent.has_value());
    QCOMPARE(keyboardAbort.replayEvents.first().key.keysym.scancode,
             SDL_SCANCODE_LCTRL);
    QCOMPARE(keyboardAbort.replayEvents.first().type, Uint32(SDL_KEYDOWN));

    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter controllerRouter(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));
    controllerRouter.openForKeyboard();
    QCOMPARE(controllerRouter.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 27, SDL_CONTROLLER_BUTTON_A)).action,
             DeckInputRouter::Action::None);
    QCOMPARE(controllerRouter.controllerOwner(), SDL_JoystickID(27));
    const auto controllerAbort = controllerRouter.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 27, SDL_CONTROLLER_BUTTON_Y));
    QCOMPARE(controllerAbort.replayEvents.size(), 1);
    QVERIFY(controllerAbort.deferredEvent.has_value());
    QCOMPARE(controllerRouter.routeReplay(
                 controllerAbort.replayEvents.first()).action,
             DeckInputRouter::Action::Activate);
    QCOMPARE(controllerAbort.action, DeckInputRouter::Action::None);
    QCOMPARE(controllerRouter.routeDeferred(
                 *controllerAbort.deferredEvent).action,
             DeckInputRouter::Action::FocusSearch);
}

void DeckInputRouterTest::controllerRemovalDiscardsOnlyItsPhysicalState()
{
    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));

    router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_A));
    router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 18, SDL_CONTROLLER_BUTTON_A));
    SDL_Event removed {};
    removed.type = SDL_CONTROLLERDEVICEREMOVED;
    removed.cdevice.type = SDL_CONTROLLERDEVICEREMOVED;
    removed.cdevice.which = 17;
    router.route(removed);

    const auto staleCompletion = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 17, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(staleCompletion.action, DeckInputRouter::Action::None);
    QVERIFY(!router.isDeckOpen());
    const auto postRemovalRelease = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONUP, 17, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(postRemovalRelease.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(postRemovalRelease.replayEvents.isEmpty());
    const auto survivingCompletion = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 18, SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(survivingCompletion.action,
             DeckInputRouter::Action::OpenFromController);
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(18));
}

void DeckInputRouterTest::reservedQuitAlwaysPassesThroughWhenDeckClosed()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    const QList<QList<SDL_GameControllerButton>> pressOrders {
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_START},
        {SDL_CONTROLLER_BUTTON_START,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    };

    for (const auto& order : pressOrders) {
        DeckInputRouter router(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_F8,
            customChord, false));
        DeckInputRouter::Result completed;
        for (SDL_GameControllerButton button : order) {
            completed = router.route(buttonEvent(
                SDL_CONTROLLERBUTTONDOWN, 301, button));
            QCOMPARE(completed.disposition,
                     DeckInputRouter::Disposition::Passthrough);
            QCOMPARE(completed.action, DeckInputRouter::Action::None);
            QVERIFY(completed.replayEvents.isEmpty());
        }
        QVERIFY(!router.isDeckOpen());
        for (SDL_GameControllerButton button : order) {
            const auto released = router.route(buttonEvent(
                SDL_CONTROLLERBUTTONUP, 301, button));
            QCOMPARE(released.disposition,
                     DeckInputRouter::Disposition::Passthrough);
            QVERIFY(released.replayEvents.isEmpty());
        }
    }

    DeckInputRouter legacyRouter(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, true));
    for (SDL_GameControllerButton button : pressOrders.first()) {
        QCOMPARE(legacyRouter.route(buttonEvent(
                     SDL_CONTROLLERBUTTONDOWN, 302, button)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
    }
}

void DeckInputRouterTest::closedStatsPassesThroughImmediatelyInEveryPressOrder()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    for (bool swapFaceButtons : {false, true}) {
        const SDL_GameControllerButton faceButton = swapFaceButtons
            ? SDL_CONTROLLER_BUTTON_Y : SDL_CONTROLLER_BUTTON_X;
        const QList<QList<SDL_GameControllerButton>> pressOrders {
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
             SDL_CONTROLLER_BUTTON_BACK,
             faceButton},
            {faceButton,
             SDL_CONTROLLER_BUTTON_BACK,
             SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        };
        for (const auto& order : pressOrders) {
            DeckInputRouter router(DeckBindings(
                int(Qt::ControlModifier), SDL_SCANCODE_F8,
                customChord, false), swapFaceButtons);
            DeckInputRouter::Result completed;
            for (int i = 0; i < order.size(); ++i) {
                completed = router.route(buttonEvent(
                    SDL_CONTROLLERBUTTONDOWN, 303, order.at(i)));
                QCOMPARE(completed.disposition,
                         DeckInputRouter::Disposition::Passthrough);
                QVERIFY(completed.replayEvents.isEmpty());
            }
            QCOMPARE(completed.action, DeckInputRouter::Action::None);
            QVERIFY(!router.isDeckOpen());
        }
    }
}

void DeckInputRouterTest::externalVisibilitySyncClearsKeyboardCandidates()
{
    for (bool startOpen : {false, true}) {
        DeckInputRouter router(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_ESCAPE,
            DeckBindings::defaultControllerButtons(), false));
        if (startOpen) {
            router.openForKeyboard();
        }
        QCOMPARE(router.route(keyEvent(
                     SDL_KEYDOWN, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE)).action,
                 DeckInputRouter::Action::None);

        router.syncDeckOpen(!startOpen);
        const auto release = router.route(keyEvent(
            SDL_KEYUP, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
        QCOMPARE(release.disposition,
                 DeckInputRouter::Disposition::Consumed);
        QVERIFY(release.replayEvents.isEmpty());

        const auto nextTap = router.route(keyEvent(
            SDL_KEYDOWN, SDL_SCANCODE_C, SDLK_c));
        QCOMPARE(nextTap.disposition,
                 startOpen ? DeckInputRouter::Disposition::Passthrough
                           : DeckInputRouter::Disposition::Consumed);
        QCOMPARE(nextTap.action,
                 startOpen ? DeckInputRouter::Action::None
                           : DeckInputRouter::Action::Key);
    }
}

void DeckInputRouterTest::externalVisibilitySyncClearsEveryControllerCandidate()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, false));
    for (SDL_JoystickID controller : {SDL_JoystickID(17), SDL_JoystickID(18)}) {
        QCOMPARE(router.route(buttonEvent(
                     SDL_CONTROLLERBUTTONDOWN, controller,
                     SDL_CONTROLLER_BUTTON_A)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
    }

    router.syncDeckOpen(true);
    for (SDL_JoystickID controller : {SDL_JoystickID(17), SDL_JoystickID(18)}) {
        const auto release = router.route(buttonEvent(
            SDL_CONTROLLERBUTTONUP, controller,
            SDL_CONTROLLER_BUTTON_A));
        QCOMPARE(release.disposition,
                 DeckInputRouter::Disposition::Consumed);
        QVERIFY(release.replayEvents.isEmpty());
    }

    router.syncDeckOpen(false);
    for (SDL_JoystickID controller : {SDL_JoystickID(17), SDL_JoystickID(18)}) {
        QCOMPARE(router.route(buttonEvent(
                     SDL_CONTROLLERBUTTONDOWN, controller,
                     SDL_CONTROLLER_BUTTON_DPAD_UP)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
        QCOMPARE(router.route(buttonEvent(
                     SDL_CONTROLLERBUTTONUP, controller,
                     SDL_CONTROLLER_BUTTON_DPAD_UP)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
    }

    for (SDL_JoystickID controller : {SDL_JoystickID(27), SDL_JoystickID(28)}) {
        DeckInputRouter openRouter(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_F8,
            customChord, false));
        openRouter.openForKeyboard();
        openRouter.route(buttonEvent(
            SDL_CONTROLLERBUTTONDOWN, controller,
            SDL_CONTROLLER_BUTTON_A));
        openRouter.syncDeckOpen(false);
        const auto release = openRouter.route(buttonEvent(
            SDL_CONTROLLERBUTTONUP, controller,
            SDL_CONTROLLER_BUTTON_A));
        QCOMPARE(release.disposition,
                 DeckInputRouter::Disposition::Consumed);
        QVERIFY(release.replayEvents.isEmpty());
        QCOMPARE(openRouter.route(buttonEvent(
                     SDL_CONTROLLERBUTTONDOWN, controller,
                     SDL_CONTROLLER_BUTTON_DPAD_UP)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
    }
}

void DeckInputRouterTest::candidateAbortDefersControllerInputUntilAfterReplay()
{
    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_B) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_X);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));
    router.openForKeyboard();

    QCOMPARE(router.route(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41,
                 SDL_CONTROLLER_BUTTON_B)).action,
             DeckInputRouter::Action::None);
    const auto abort = router.route(buttonEvent(
        SDL_CONTROLLERBUTTONDOWN, 41, SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(abort.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(abort.action, DeckInputRouter::Action::None);
    QCOMPARE(abort.replayEvents.size(), 1);
    QVERIFY(abort.deferredEvent.has_value());
    QCOMPARE(abort.deferredEvent->cbutton.button,
             Uint8(SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(router.routeReplay(abort.replayEvents.first()).action,
             DeckInputRouter::Action::Back);

    router.syncDeckOpen(false);
    QCOMPARE(router.routeReplay(buttonEvent(
                 SDL_CONTROLLERBUTTONDOWN, 41,
                 SDL_CONTROLLER_BUTTON_A)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::candidateAbortDefersKeyboardInputUntilAfterReplay()
{
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_ESCAPE,
        DeckBindings::defaultControllerButtons(), false));
    router.openForKeyboard();

    QCOMPARE(router.route(keyEvent(
                 SDL_KEYDOWN, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE)).action,
             DeckInputRouter::Action::None);
    const auto abort = router.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN));
    QCOMPARE(abort.disposition, DeckInputRouter::Disposition::Consumed);
    QCOMPARE(abort.action, DeckInputRouter::Action::None);
    QCOMPARE(abort.replayEvents.size(), 1);
    QVERIFY(abort.deferredEvent.has_value());
    QCOMPARE(abort.deferredEvent->key.keysym.scancode,
             SDL_SCANCODE_RETURN);
    QCOMPARE(router.routeReplay(abort.replayEvents.first()).action,
             DeckInputRouter::Action::Key);

    router.syncDeckOpen(false);
    QCOMPARE(router.routeReplay(keyEvent(
                 SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN)).disposition,
             DeckInputRouter::Disposition::Consumed);
}

void DeckInputRouterTest::closedOrdinaryHoldsPassThroughImmediatelyWithoutReplay()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);

    DeckInputRouter keyboardRouter(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_SPACE,
        customChord, false));
    const auto spaceDown = keyboardRouter.route(keyEvent(
        SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE));
    QCOMPARE(spaceDown.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(spaceDown.replayEvents.isEmpty());
    const auto spaceUp = keyboardRouter.route(keyEvent(
        SDL_KEYUP, SDL_SCANCODE_SPACE, SDLK_SPACE));
    QCOMPARE(spaceUp.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(spaceUp.replayEvents.isEmpty());

    for (SDL_GameControllerButton button : {
             SDL_CONTROLLER_BUTTON_X,
             SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
             SDL_CONTROLLER_BUTTON_BACK,
             SDL_CONTROLLER_BUTTON_START,
         }) {
        DeckInputRouter controllerRouter(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_SPACE,
            customChord, false));
        const auto down = controllerRouter.route(buttonEvent(
            SDL_CONTROLLERBUTTONDOWN, 501, button));
        QCOMPARE(down.disposition,
                 DeckInputRouter::Disposition::Passthrough);
        QVERIFY(down.replayEvents.isEmpty());
        const auto up = controllerRouter.route(buttonEvent(
            SDL_CONTROLLERBUTTONUP, 501, button));
        QCOMPARE(up.disposition,
                 DeckInputRouter::Disposition::Passthrough);
        QVERIFY(up.replayEvents.isEmpty());
    }
}

void DeckInputRouterTest::closedKeyboardRepeatsStayImmediateAndBounded()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_SPACE,
        customChord, false));

    for (int repeat = 0; repeat < 256; ++repeat) {
        const auto result = router.route(keyEvent(
            SDL_KEYDOWN, SDL_SCANCODE_SPACE, SDLK_SPACE,
            KMOD_NONE, repeat == 0 ? 0 : 1));
        QCOMPARE(result.disposition,
                 DeckInputRouter::Disposition::Passthrough);
        QVERIFY(result.replayEvents.isEmpty());
    }
    const auto released = router.route(keyEvent(
        SDL_KEYUP, SDL_SCANCODE_SPACE, SDLK_SPACE));
    QCOMPARE(released.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(released.replayEvents.isEmpty());
}

void DeckInputRouterTest::openLegacyDisconnectChordWinsBeforeLocalNavigation()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    for (bool swapFaceButtons : {false, true}) {
        DeckInputRouter router(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_F8,
            customChord, true), swapFaceButtons);
        router.openForKeyboard();

        DeckInputRouter::Result completed;
        for (SDL_GameControllerButton button : {
                 SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                 SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                 SDL_CONTROLLER_BUTTON_BACK,
                 SDL_CONTROLLER_BUTTON_START,
             }) {
            completed = router.route(buttonEvent(
                SDL_CONTROLLERBUTTONDOWN, 502, button));
            QCOMPARE(completed.disposition,
                     DeckInputRouter::Disposition::Consumed);
        }
        QCOMPARE(completed.action,
                 DeckInputRouter::Action::LegacyDisconnect);
        QVERIFY(router.isDeckOpen());
        for (SDL_GameControllerButton button : {
                 SDL_CONTROLLER_BUTTON_START,
                 SDL_CONTROLLER_BUTTON_BACK,
                 SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                 SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             }) {
            QCOMPARE(router.route(buttonEvent(
                         SDL_CONTROLLERBUTTONUP, 502, button)).disposition,
                     DeckInputRouter::Disposition::Consumed);
        }
    }
}

REGISTER_PERIGEE_TEST(DeckInputRouterTest);

#include "test_deckinputrouter.moc"
