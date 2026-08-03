#include "test_registry.h"
#include "input_integration_stubs.h"

#include <QtTest>

#include <QProcess>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#define private public
#include "settings/streamingpreferences.h"
#include "streaming/input/input.h"
#include "streaming/sdleventcodes.h"
#include "perigee/input/deckinputrouter.h"
#undef private

namespace {

void initializePreferences(StreamingPreferences& preferences)
{
    preferences.multiController = true;
    preferences.gamepadMouse = false;
    preferences.swapMouseButtons = false;
    preferences.reverseScrollDirection = false;
    preferences.swapFaceButtons = false;
    preferences.backgroundGamepad = false;
    preferences.absoluteMouseMode = false;
    preferences.absoluteTouchMode = false;
    preferences.captureSysKeysMode = StreamingPreferences::CSK_OFF;
    const DeckBindings defaults;
    preferences.deckKeyModifiers = defaults.keyModifiers();
    preferences.deckKeyScancode = defaults.keyScancode();
    preferences.deckControllerButtons = int(defaults.controllerButtons());
    preferences.legacyGamepadDisconnect = false;
}

SDL_UserEvent inputTimerEvent(uint32_t token)
{
    SDL_UserEvent event {};
    event.type = SDL_USEREVENT;
    event.code = SDL_CODE_INPUT_TIMER;
    event.data1 = reinterpret_cast<void*>(uintptr_t(token));
    return event;
}

bool dispatchNextInputTimerEvent(SdlInputHandler& handler, int timeoutMs = 1000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        SDL_Event event {};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_USEREVENT &&
                    event.user.code == SDL_CODE_INPUT_TIMER) {
                return handler.handleInputTimerEvent(event.user);
            }
        }
        QTest::qWait(1);
    } while (elapsed.elapsed() < timeoutMs);
    return false;
}

SDL_Event controllerButtonEvent(Uint32 type, SDL_JoystickID controller,
                                Uint8 button)
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

DeckInputRouter::Result routeThroughRealHandler(
        DeckInputRouter& router, SdlInputHandler& handler, SDL_Event event)
{
    const DeckInputRouter::Result original = router.route(event);
    DeckInputRouter::Result result = original;
    while (true) {
        for (SDL_Event replay : result.replayEvents) {
            if (result.replayToDeck) {
                router.routeReplay(replay);
            }
            else if (replay.type == SDL_CONTROLLERBUTTONDOWN ||
                     replay.type == SDL_CONTROLLERBUTTONUP) {
                handler.handleControllerButtonEvent(&replay.cbutton);
            }
            else if (replay.type == SDL_KEYDOWN || replay.type == SDL_KEYUP) {
                handler.handleKeyEvent(&replay.key);
            }
        }

        if (result.action == DeckInputRouter::Action::OpenFromController ||
                result.action == DeckInputRouter::Action::OpenFromKeyboard) {
            handler.beginLocalOverlayInput();
        }
        else if (result.action == DeckInputRouter::Action::ToggleStats) {
            handler.sendNeutralControllerInput(result.controllerId);
        }
        else if (result.action ==
                 DeckInputRouter::Action::LegacyDisconnect) {
            handler.handleLegacyGamepadDisconnect(result.controllerId);
        }
        if (!result.deferredEvent.has_value()) {
            break;
        }
        SDL_Event deferred = *result.deferredEvent;
        result = router.routeDeferred(deferred);
        if (result.disposition == DeckInputRouter::Disposition::Passthrough) {
            if (deferred.type == SDL_CONTROLLERBUTTONDOWN ||
                    deferred.type == SDL_CONTROLLERBUTTONUP) {
                handler.handleControllerButtonEvent(&deferred.cbutton);
            }
            else if (deferred.type == SDL_KEYDOWN ||
                     deferred.type == SDL_KEYUP) {
                handler.handleKeyEvent(&deferred.key);
            }
            break;
        }
    }

    if (original.disposition == DeckInputRouter::Disposition::Passthrough) {
        if (event.type == SDL_CONTROLLERBUTTONDOWN ||
                event.type == SDL_CONTROLLERBUTTONUP) {
            handler.handleControllerButtonEvent(&event.cbutton);
        }
        else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            handler.handleKeyEvent(&event.key);
        }
    }
    return original;
}

}

class InputIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void relativeTapReleaseTimerUsesTheOwningHandler();
    void timerCallbacksOnlyEnqueueAndDoNotTouchTheHandler();
    void destructionCanFinishWhileDragTimerCallbackIsInFlight();
    void nativeInputIsCancelledBeforeDeckOwnsInput();
    void ownershipWaitsForInFlightTimerSendBeforeNeutralizing();
    void takeoverRotatesGamepadTimerTokenAndDropsQueuedWork();
    void gamepadMouseDispatchReadsAxisStateOnMainThread();
    void gamepadMouseSelectsFullNegativeStickWithoutOverflow();
    void gamepadTimerAddFailureDoesNotAdvertiseMouseMode();
    void staleTimerTokenIsIgnoredAfterHandlerReplacement();
    void neutralRemoteInputPrecedesPhysicalCaptureRelease();
    void startupCaptureRequestsAreDeferredWhileDeckIsOpen();
    void statsChordNeutralizesOnlyItsController();
    void statsChordPreservesOtherPhysicalControllerInMergedMode();
    void closedStatsChordUsesRemoteHandlerAndReleasesGamepadMouseButtons();
    void routerRunsBeforeDeviceAndBatteryHousekeeping();
    void safeDeckChordNeverReachesLegacyQuitAcrossSwapAndPressOrders();
    void closedIncompleteCustomChordStaysImmediateAndBalanced();
    void legacyToggleOwnsReservedQuitWithCustomDeckChord();
    void closedUnrelatedInputNeverUsesReservedQuitReplay();
    void localBackReplayDiscardsRemainingBufferAndReroutesCurrentToHost();
    void closedStartHoldReachesMouseModeTimingImmediately();
    void closedConfiguredChordNeutralizesForwardedPrefixState();
    void legacyOffIgnoresReservedQuitAfterUnrelatedButtonRelease();
    void legacyDisconnectHelperBypassesOverlayGate();
    void legacyOnOpenDisconnectsThroughRealHandler();
    void nonOwnerLegacyChordCannotQuitWhileDeckIsOwned();
};

void InputIntegrationTest::init()
{
    SDL_FlushEvent(SDL_USEREVENT);
    InputIntegrationStubs::reset();
}

void InputIntegrationTest::relativeTapReleaseTimerUsesTheOwningHandler()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);

    handler.m_TouchDownEvent[0].fingerId = 17;
    handler.m_TouchDownEvent[0].timestamp = 400;
    SDL_TouchFingerEvent release {};
    release.type = SDL_FINGERUP;
    release.touchId = SDL_TOUCH_MOUSEID;
    release.fingerId = 17;
    release.timestamp = 500;
    handler.handleRelativeFingerEvent(&release);
    QVERIFY(dispatchNextInputTimerEvent(handler));

    QCOMPARE(InputIntegrationStubs::mouseButtons().size(), 2);
    const auto records = InputIntegrationStubs::mouseButtons();
    QCOMPARE(records.at(0).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(records.at(0).button, BUTTON_LEFT);
    QCOMPARE(records.at(1).action, int(BUTTON_ACTION_RELEASE));
    QCOMPARE(records.at(1).button, BUTTON_LEFT);
}

void InputIntegrationTest::timerCallbacksOnlyEnqueueAndDoNotTouchTheHandler()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_NumFingersDown = 1;
    GamepadState gamepad {};
    gamepad.lsX = 32767;

    InputIntegrationStubs::failNextInputTimerPush();
    QCOMPARE(SdlInputHandler::dragTimerCallback(23, &handler), Uint32(23));

    const QVector<std::function<Uint32()>> callbacks {
        [&handler] { return SdlInputHandler::longPressTimerCallback(1, &handler); },
        [&handler] { return SdlInputHandler::releaseLeftButtonTimerCallback(1, &handler); },
        [&handler] { return SdlInputHandler::releaseRightButtonTimerCallback(1, &handler); },
        [&handler] { return SdlInputHandler::dragTimerCallback(1, &handler); },
        [&gamepad] { return SdlInputHandler::mouseEmulationTimerCallback(1, &gamepad); },
    };

    QVector<InputIntegrationStubs::TimerCallbackBlock> blockedAt;
    for (const auto& callback : callbacks) {
        InputIntegrationStubs::blockNextTimerCallbackSideEffect();
        std::thread timer([&callback] { callback(); });
        blockedAt.push_back(
            InputIntegrationStubs::waitUntilTimerCallbackBlocked());
        InputIntegrationStubs::releaseBlockedSend();
        timer.join();
    }

    QCOMPARE(blockedAt,
             QVector<InputIntegrationStubs::TimerCallbackBlock>(
                 callbacks.size(),
                 InputIntegrationStubs::TimerCallbackBlock::InputEventPush));
    QCOMPARE(InputIntegrationStubs::mouseButtons().size(), 0);
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 0);
}

void InputIntegrationTest::destructionCanFinishWhileDragTimerCallbackIsInFlight()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    auto handler = std::make_unique<SdlInputHandler>(preferences, 1920, 1080);
    InputIntegrationStubs::blockNextTimerCallbackSideEffect();
    handler->startInputTimer(handler->m_DragTimer,
                             handler->m_DragTimerToken,
                             1,
                             SdlInputHandler::InputTimerAction::Drag);
    QVERIFY(handler->m_DragTimer != 0);
    const auto blockedAt =
        InputIntegrationStubs::waitUntilTimerCallbackBlocked();

    bool destroyedWhileCallbackBlocked = false;
    if (blockedAt == InputIntegrationStubs::TimerCallbackBlock::InputEventPush) {
        std::atomic_bool destroyed {false};
        std::thread destroyer([&handler, &destroyed] {
            handler.reset();
            destroyed.store(true);
        });
        for (int i = 0; i < 100 && !destroyed.load(); ++i) {
            QTest::qWait(1);
        }
        destroyedWhileCallbackBlocked = destroyed.load();
        InputIntegrationStubs::releaseBlockedSend();
        QVERIFY(InputIntegrationStubs::waitUntilTimerCallbackReleased());
        destroyer.join();
    }
    else {
        InputIntegrationStubs::releaseBlockedSend();
        handler.reset();
    }

    QCOMPARE(blockedAt,
             InputIntegrationStubs::TimerCallbackBlock::InputEventPush);
    QVERIFY(destroyedWhileCallbackBlocked);
    QCOMPARE(InputIntegrationStubs::mouseButtons().size(), 0);
}

void InputIntegrationTest::nativeInputIsCancelledBeforeDeckOwnsInput()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);

    handler.sendTrackedTouchEvent(LI_TOUCH_EVENT_DOWN, 41, .25f, .5f, 1.0f);
    handler.sendTrackedPenEvent(LI_TOUCH_EVENT_DOWN, .4f, .6f, 1.0f);
    handler.beginLocalOverlayInput();

    const auto touches = InputIntegrationStubs::touches();
    QCOMPARE(touches.size(), 2);
    QCOMPARE(touches.at(1).eventType, int(LI_TOUCH_EVENT_CANCEL_ALL));
    const auto pens = InputIntegrationStubs::pens();
    QCOMPARE(pens.size(), 2);
    QCOMPARE(pens.at(1).eventType, int(LI_TOUCH_EVENT_CANCEL));

    handler.sendTrackedTouchEvent(LI_TOUCH_EVENT_UP, 41, .25f, .5f, 0.0f);
    handler.sendTrackedPenEvent(LI_TOUCH_EVENT_UP, .4f, .6f, 0.0f);
    QCOMPARE(InputIntegrationStubs::touches().size(), 2);
    QCOMPARE(InputIntegrationStubs::pens().size(), 2);
}

void InputIntegrationTest::ownershipWaitsForInFlightTimerSendBeforeNeutralizing()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    InputIntegrationStubs::blockNextMouseButtonSend();

    std::thread timer([&handler] {
        handler.sendTrackedMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    });
    QVERIFY(InputIntegrationStubs::waitUntilSendBlocked());
    std::thread owner([&handler] { handler.beginLocalOverlayInput(); });
    QTRY_VERIFY_WITH_TIMEOUT(handler.m_LocalOverlayInputActive.load(), 1000);
    std::thread lateTimer([&handler] {
        handler.sendTrackedMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
    });

    InputIntegrationStubs::releaseBlockedSend();
    timer.join();
    owner.join();
    lateTimer.join();

    const auto records = InputIntegrationStubs::mouseButtons();
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(records.at(0).button, BUTTON_LEFT);
    QCOMPARE(records.at(1).action, int(BUTTON_ACTION_RELEASE));
    QCOMPARE(records.at(1).button, BUTTON_LEFT);
}

void InputIntegrationTest::takeoverRotatesGamepadTimerTokenAndDropsQueuedWork()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 44;
    state.index = 0;
    state.lsX = 32767;
    handler.m_GamepadMask = 1;
    handler.startInputTimer(state.mouseEmulationTimer,
                            state.mouseEmulationTimerToken,
                            100000,
                            SdlInputHandler::InputTimerAction::MouseEmulation,
                            state.jsId,
                            true);
    const uint32_t oldToken = state.mouseEmulationTimerToken;
    QVERIFY(oldToken != 0);

    const CaptureSnapshot snapshot = handler.beginLocalOverlayInput();
    const uint32_t deckToken = state.mouseEmulationTimerToken;
    QVERIFY(deckToken != 0);
    QVERIFY(deckToken != oldToken);
    QVERIFY(state.mouseEmulationTimer != 0);
    QVERIFY(!handler.m_InputTimerRequests.contains(oldToken));
    QVERIFY(handler.m_InputTimerRequests.contains(deckToken));

    QVERIFY(handler.handleInputTimerEvent(inputTimerEvent(oldToken)));
    QVERIFY(handler.handleInputTimerEvent(inputTimerEvent(deckToken)));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 0);

    handler.endLocalOverlayInput(snapshot, false);
    const uint32_t remoteToken = state.mouseEmulationTimerToken;
    QVERIFY(remoteToken != 0);
    QVERIFY(remoteToken != deckToken);
    state.lsX = 32767;
    QVERIFY(handler.handleInputTimerEvent(inputTimerEvent(oldToken)));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 0);
    QVERIFY(handler.handleInputTimerEvent(inputTimerEvent(deckToken)));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 0);
    QVERIFY(handler.handleInputTimerEvent(inputTimerEvent(remoteToken)));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 1);

    handler.cancelInputTimer(state.mouseEmulationTimer,
                             state.mouseEmulationTimerToken);
    state.controller = nullptr;
}

void InputIntegrationTest::gamepadTimerAddFailureDoesNotAdvertiseMouseMode()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.gamepadMouse = true;
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 45;
    state.index = 0;
    state.lastStartDownTime = SDL_GetTicks() - 751;
    handler.m_GamepadMask = 1;

    InputIntegrationStubs::failNextTimerAdd();
    SDL_ControllerButtonEvent event {};
    event.type = SDL_CONTROLLERBUTTONUP;
    event.which = state.jsId;
    event.button = SDL_CONTROLLER_BUTTON_START;
    event.state = SDL_RELEASED;
    handler.handleControllerButtonEvent(&event);

    QCOMPARE(state.mouseEmulationTimer, SDL_TimerID(0));
    QCOMPARE(state.mouseEmulationTimerToken, uint32_t(0));
    QCOMPARE(InputIntegrationStubs::mouseEmulationNotifications().size(), 0);
    state.controller = nullptr;
}

void InputIntegrationTest::gamepadMouseDispatchReadsAxisStateOnMainThread()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 44;
    state.index = 0;
    handler.m_GamepadMask = 1;
    handler.startInputTimer(state.mouseEmulationTimer,
                            state.mouseEmulationTimerToken,
                            100000,
                            SdlInputHandler::InputTimerAction::MouseEmulation,
                            state.jsId,
                            true);

    SDL_ControllerAxisEvent axis {};
    axis.type = SDL_CONTROLLERAXISMOTION;
    axis.which = 44;
    axis.axis = SDL_CONTROLLER_AXIS_LEFTX;
    axis.value = 32767;
    handler.handleControllerAxisEvent(&axis);
    QCOMPARE(state.lsX, short(32767));
    QVERIFY(handler.handleInputTimerEvent(
        inputTimerEvent(state.mouseEmulationTimerToken)));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 1);

    handler.cancelInputTimer(state.mouseEmulationTimer,
                             state.mouseEmulationTimerToken);
    state.controller = nullptr;
}

void InputIntegrationTest::gamepadMouseSelectsFullNegativeStickWithoutOverflow()
{
    if (qEnvironmentVariableIsEmpty("PERIGEE_NEGATIVE_AXIS_CHILD")) {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PERIGEE_NEGATIVE_AXIS_CHILD"),
                           QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("InputIntegrationTest"),
                     QStringLiteral("gamepadMouseSelectsFullNegativeStickWithoutOverflow"),
                     QStringLiteral("-silent")});
        QVERIFY2(child.waitForFinished(5000), qPrintable(child.errorString()));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        return;
    }

    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 46;
    state.index = 0;
    handler.m_GamepadMask = 1;
    handler.startInputTimer(state.mouseEmulationTimer,
                            state.mouseEmulationTimerToken,
                            100000,
                            SdlInputHandler::InputTimerAction::MouseEmulation,
                            state.jsId,
                            true);

    for (const auto axisValue : {
             std::pair<SDL_GameControllerAxis, Sint16> {
                 SDL_CONTROLLER_AXIS_LEFTX, 32767},
             {SDL_CONTROLLER_AXIS_LEFTY, 32767},
             {SDL_CONTROLLER_AXIS_RIGHTX, -32768},
             {SDL_CONTROLLER_AXIS_RIGHTY, -32768},
         }) {
        SDL_ControllerAxisEvent axis {};
        axis.type = SDL_CONTROLLERAXISMOTION;
        axis.which = state.jsId;
        axis.axis = axisValue.first;
        axis.value = axisValue.second;
        handler.handleControllerAxisEvent(&axis);
    }

    QCOMPARE(state.lsX, short(32767));
    QCOMPARE(state.lsY, short(-32767));
    QCOMPARE(state.rsX, short(-32768));
    QCOMPARE(state.rsY, short(32767));

    QVERIFY(handler.handleInputTimerEvent(
        inputTimerEvent(state.mouseEmulationTimerToken)));
    const auto moves = InputIntegrationStubs::mouseMoves();
    QCOMPARE(moves.size(), 1);
    QVERIFY(moves.first().x < 0);
    QVERIFY(moves.first().y < 0);

    handler.cancelInputTimer(state.mouseEmulationTimer,
                             state.mouseEmulationTimerToken);
    state.controller = nullptr;
}

void InputIntegrationTest::staleTimerTokenIsIgnoredAfterHandlerReplacement()
{
    StreamingPreferences firstPreferences(nullptr);
    initializePreferences(firstPreferences);
    auto first = std::make_unique<SdlInputHandler>(
        firstPreferences, 1920, 1080);
    first->startInputTimer(first->m_LeftButtonReleaseTimer,
                           first->m_LeftButtonReleaseTimerToken,
                           100000,
                           SdlInputHandler::InputTimerAction::ReleaseLeftButton);
    const uint32_t staleToken = first->m_LeftButtonReleaseTimerToken;
    QVERIFY(staleToken != 0);
    first.reset();

    StreamingPreferences secondPreferences(nullptr);
    initializePreferences(secondPreferences);
    SdlInputHandler second(secondPreferences, 1920, 1080);
    second.startInputTimer(second.m_LeftButtonReleaseTimer,
                           second.m_LeftButtonReleaseTimerToken,
                           100000,
                           SdlInputHandler::InputTimerAction::ReleaseLeftButton);
    QVERIFY(second.m_LeftButtonReleaseTimerToken != staleToken);

    QVERIFY(second.handleInputTimerEvent(inputTimerEvent(staleToken)));
    QCOMPARE(InputIntegrationStubs::mouseButtons().size(), 0);
}

void InputIntegrationTest::neutralRemoteInputPrecedesPhysicalCaptureRelease()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    SDL_Window* window = SDL_CreateWindow("capture-order", 0, 0, 640, 360,
                                          SDL_WINDOW_HIDDEN);
    QVERIFY(window != nullptr);
    handler.setWindow(window);
    handler.setCaptureActive(true);
    QVERIFY(handler.isCaptureActive());
    handler.sendTrackedMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);

    InputIntegrationStubs::beginOrderingObservation();
    handler.beginLocalOverlayInput();
    QCOMPARE(InputIntegrationStubs::ordering(),
             QStringList({QStringLiteral("remote-mouse-release"),
                          QStringLiteral("capture-off")}));

    handler.setWindow(nullptr);
    SDL_DestroyWindow(window);
}

void InputIntegrationTest::startupCaptureRequestsAreDeferredWhileDeckIsOpen()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.absoluteMouseMode = true;
    SdlInputHandler handler(preferences, 1920, 1080);
    SDL_Window* window = SDL_CreateWindow("capture", 0, 0, 640, 360, SDL_WINDOW_HIDDEN);
    QVERIFY(window != nullptr);
    handler.setWindow(window);

    handler.beginLocalOverlayInput();
    handler.setCaptureActive(true); // SDL_WINDOWEVENT_ENTER
    QVERIFY(!handler.isCaptureActive());
    handler.setCaptureActive(true); // post-decoder recreation
    QVERIFY(!handler.isCaptureActive());
    handler.endLocalOverlayInput({false, false}, false);
    QVERIFY(handler.isCaptureActive());
    handler.setCaptureActive(false);
    handler.setWindow(nullptr);
    SDL_DestroyWindow(window);
}

void InputIntegrationTest::statsChordNeutralizesOnlyItsController()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_GamepadMask = 0x3;
    handler.m_GamepadState[0].controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    handler.m_GamepadState[0].jsId = 70;
    handler.m_GamepadState[0].index = 0;
    handler.m_GamepadState[0].buttons = LB_FLAG | RB_FLAG | BACK_FLAG | X_FLAG;
    handler.m_GamepadState[0].lsX = 1234;
    handler.m_GamepadState[1].controller = reinterpret_cast<SDL_GameController*>(quintptr(2));
    handler.m_GamepadState[1].jsId = 71;
    handler.m_GamepadState[1].index = 1;
    handler.m_GamepadState[1].buttons = A_FLAG;
    handler.m_GamepadState[1].lsX = 2222;
    handler.m_RemoteInputState.keySent(12, true);
    handler.m_RemoteInputState.mouseButtonSent(BUTTON_RIGHT, true);

    QVERIFY(handler.sendNeutralControllerInput(70));

    QCOMPARE(handler.m_GamepadState[0].buttons, 0);
    QCOMPARE(handler.m_GamepadState[0].lsX, 0);
    QCOMPARE(handler.m_GamepadState[1].buttons, A_FLAG);
    QCOMPARE(handler.m_GamepadState[1].lsX, 2222);
    QVERIFY(handler.m_RemoteInputState.hasKeysDown());
    QVERIFY(handler.m_RemoteInputState.hasMouseButtonsDown());
    QCOMPARE(InputIntegrationStubs::mouseButtons().size(), 0);
    const auto controllers = InputIntegrationStubs::controllers();
    QCOMPARE(controllers.size(), 1);
    QCOMPARE(controllers.at(0).controllerIndex, 0);
    QCOMPARE(controllers.at(0).buttons, 0);

    handler.m_GamepadState[0].controller = nullptr;
    handler.m_GamepadState[1].controller = nullptr;
}

void InputIntegrationTest::statsChordPreservesOtherPhysicalControllerInMergedMode()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.multiController = false;
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_GamepadMask = 0x1;
    handler.m_GamepadState[0].controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    handler.m_GamepadState[0].jsId = 70;
    handler.m_GamepadState[0].index = 0;
    handler.m_GamepadState[0].buttons = LB_FLAG | RB_FLAG | BACK_FLAG | X_FLAG;
    handler.m_GamepadState[0].lsX = 1234;
    handler.m_GamepadState[1].controller = reinterpret_cast<SDL_GameController*>(quintptr(2));
    handler.m_GamepadState[1].jsId = 71;
    handler.m_GamepadState[1].index = 0;
    handler.m_GamepadState[1].buttons = A_FLAG;
    handler.m_GamepadState[1].lsX = 2222;

    QVERIFY(handler.sendNeutralControllerInput(70));

    QCOMPARE(handler.m_GamepadState[0].buttons, 0);
    QCOMPARE(handler.m_GamepadState[1].buttons, A_FLAG);
    const auto controllers = InputIntegrationStubs::controllers();
    QCOMPARE(controllers.size(), 1);
    QCOMPARE(controllers.at(0).controllerIndex, 0);
    QCOMPARE(controllers.at(0).buttons, A_FLAG);
    QCOMPARE(controllers.at(0).leftStickX, 2222);

    handler.m_GamepadState[0].controller = nullptr;
    handler.m_GamepadState[1].controller = nullptr;
}

void InputIntegrationTest::closedStatsChordUsesRemoteHandlerAndReleasesGamepadMouseButtons()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    for (bool swapFaceButtons : {false, true}) {
        const Uint8 faceButton = swapFaceButtons
            ? SDL_CONTROLLER_BUTTON_Y : SDL_CONTROLLER_BUTTON_X;
        const QList<QList<Uint8>> pressOrders {
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
            InputIntegrationStubs::reset();
            StreamingPreferences preferences(nullptr);
            initializePreferences(preferences);
            preferences.swapFaceButtons = swapFaceButtons;
            SdlInputHandler handler(preferences, 1920, 1080);
            handler.m_GamepadMask = 0x1;
            handler.m_GamepadState[0].controller =
                reinterpret_cast<SDL_GameController*>(quintptr(1));
            handler.m_GamepadState[0].jsId = 70;
            handler.m_GamepadState[0].index = 0;
            handler.m_GamepadState[0].mouseEmulationTimer = SDL_TimerID(1);
            DeckInputRouter router(DeckBindings(
                int(Qt::ControlModifier), SDL_SCANCODE_F8,
                customChord, false), swapFaceButtons);

            DeckInputRouter::Result completed;
            for (int i = 0; i < order.size(); ++i) {
                completed = routeThroughRealHandler(
                    router, handler,
                    controllerButtonEvent(
                        SDL_CONTROLLERBUTTONDOWN, 70, order.at(i)));
                QCOMPARE(completed.disposition,
                         DeckInputRouter::Disposition::Passthrough);
                QVERIFY(completed.replayEvents.isEmpty());
            }
            QCOMPARE(InputIntegrationStubs::mouseButtons().size(), 3);

            for (Uint8 button : order) {
                QCOMPARE(routeThroughRealHandler(
                             router, handler,
                             controllerButtonEvent(
                                 SDL_CONTROLLERBUTTONUP, 70, button)).disposition,
                         DeckInputRouter::Disposition::Passthrough);
            }

            const auto mouseButtons = InputIntegrationStubs::mouseButtons();
            QCOMPARE(mouseButtons.size(), 6);
            for (int button : {BUTTON_X1, BUTTON_X2, BUTTON_MIDDLE}) {
                int presses = 0;
                int releases = 0;
                for (const auto& record : mouseButtons) {
                    if (record.button == button &&
                            record.action == int(BUTTON_ACTION_PRESS)) {
                        ++presses;
                    }
                    if (record.button == button &&
                            record.action == int(BUTTON_ACTION_RELEASE)) {
                        ++releases;
                    }
                }
                QCOMPARE(presses, 1);
                QCOMPARE(releases, 1);
            }
            QVERIFY(!handler.m_RemoteInputState.hasMouseButtonsDown());

            handler.m_GamepadState[0].mouseEmulationTimer = 0;
            handler.m_GamepadState[0].controller = nullptr;
        }
    }
}

void InputIntegrationTest::routerRunsBeforeDeviceAndBatteryHousekeeping()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_GamepadMask = 1;
    GamepadState& state = handler.m_GamepadState[0];
    state.jsId = 31;
    state.index = 0;
    state.lsX = 32767;
    handler.m_RemoteInputState.controllerAllocated(0);
    handler.startInputTimer(state.mouseEmulationTimer,
                            state.mouseEmulationTimerToken,
                            100000,
                            SdlInputHandler::InputTimerAction::MouseEmulation,
                            state.jsId,
                            true);
    const uint32_t removedTimerToken = state.mouseEmulationTimerToken;
    DeckInputRouter router;
    router.openForKeyboard();

#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_Event battery {};
    battery.type = SDL_JOYBATTERYUPDATED;
    battery.jbattery.type = SDL_JOYBATTERYUPDATED;
    battery.jbattery.which = 31;
    battery.jbattery.level = SDL_JOYSTICK_POWER_LOW;
    const auto batteryRoute = router.route(battery);
    QCOMPARE(batteryRoute.disposition, DeckInputRouter::Disposition::Passthrough);
    handler.handleJoystickBatteryEvent(&battery.jbattery);
    const auto batteries = InputIntegrationStubs::batteries();
    QCOMPARE(batteries.size(), 1);
    QCOMPARE(batteries.at(0).controllerIndex, 0);
    QCOMPARE(batteries.at(0).percentage, 20);
#endif

    SDL_Event removed {};
    removed.type = SDL_CONTROLLERDEVICEREMOVED;
    removed.cdevice.type = SDL_CONTROLLERDEVICEREMOVED;
    removed.cdevice.which = 31;
    const auto removalRoute = router.route(removed);
    QCOMPARE(removalRoute.disposition, DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(router.controllerOwner(), SDL_JoystickID(-1));
    handler.handleControllerDeviceEvent(&removed.cdevice);
    QCOMPARE(InputIntegrationStubs::controllers().size(), 1);
    QCOMPARE(InputIntegrationStubs::controllers().at(0).buttons, 0);
    QCOMPARE(state.jsId, SDL_JoystickID(0));
    QVERIFY(!handler.m_InputTimerRequests.contains(removedTimerToken));
    QVERIFY(handler.handleInputTimerEvent(inputTimerEvent(removedTimerToken)));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 0);
}

void InputIntegrationTest::safeDeckChordNeverReachesLegacyQuitAcrossSwapAndPressOrders()
{
    const QList<QList<Uint8>> pressOrders {
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_START},
        {SDL_CONTROLLER_BUTTON_START,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    };

    for (bool swapFaceButtons : {false, true}) {
        for (const auto& order : pressOrders) {
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            InputIntegrationStubs::reset();
            StreamingPreferences preferences(nullptr);
            initializePreferences(preferences);
            preferences.swapFaceButtons = swapFaceButtons;
            SdlInputHandler handler(preferences, 1920, 1080);
            GamepadState& state = handler.m_GamepadState[0];
            state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
            state.jsId = 90;
            state.index = 0;
            handler.m_GamepadMask = 1;
            handler.m_RemoteInputState.controllerAllocated(0);
            DeckInputRouter router(DeckBindings(), swapFaceButtons);

            DeckInputRouter::Result result;
            for (Uint8 button : order) {
                result = routeThroughRealHandler(
                    router, handler,
                    controllerButtonEvent(
                        SDL_CONTROLLERBUTTONDOWN, state.jsId, button));
            }
            QCOMPARE(result.action,
                     DeckInputRouter::Action::OpenFromController);
            QVERIFY(router.isDeckOpen());
            const auto records = InputIntegrationStubs::controllers();
            QVERIFY(records.size() >= 2);
            QVERIFY(records.first().buttons != 0);
            QCOMPARE(records.last().buttons, 0);

            bool sawQuit = false;
            SDL_Event queued {};
            while (SDL_PollEvent(&queued)) {
                sawQuit |= queued.type == SDL_QUIT;
            }
            QVERIFY(!sawQuit);
            state.controller = nullptr;
        }
    }
}

void InputIntegrationTest::closedIncompleteCustomChordStaysImmediateAndBalanced()
{
    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    for (bool swapFaceButtons : {false, true}) {
        InputIntegrationStubs::reset();
        StreamingPreferences preferences(nullptr);
        initializePreferences(preferences);
        preferences.swapFaceButtons = swapFaceButtons;
        SdlInputHandler handler(preferences, 1920, 1080);
        GamepadState& state = handler.m_GamepadState[0];
        state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
        state.jsId = 91;
        state.index = 0;
        state.mouseEmulationTimer = SDL_TimerID(1);
        handler.m_GamepadMask = 1;
        DeckInputRouter router(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false),
            swapFaceButtons);

        const auto down = routeThroughRealHandler(
            router, handler,
            controllerButtonEvent(
                SDL_CONTROLLERBUTTONDOWN, state.jsId,
                SDL_CONTROLLER_BUTTON_A));
        QCOMPARE(down.disposition,
                 DeckInputRouter::Disposition::Passthrough);
        const auto released = routeThroughRealHandler(
            router, handler,
            controllerButtonEvent(
                SDL_CONTROLLERBUTTONUP, state.jsId,
                SDL_CONTROLLER_BUTTON_A));
        QCOMPARE(released.disposition,
                 DeckInputRouter::Disposition::Passthrough);
        QVERIFY(released.replayEvents.isEmpty());
        const auto mouse = InputIntegrationStubs::mouseButtons();
        QCOMPARE(mouse.size(), 2);
        QCOMPARE(mouse.at(0).action, int(BUTTON_ACTION_PRESS));
        QCOMPARE(mouse.at(1).action, int(BUTTON_ACTION_RELEASE));
        const int expectedButton = swapFaceButtons ? BUTTON_RIGHT : BUTTON_LEFT;
        QCOMPARE(mouse.at(0).button, expectedButton);
        QCOMPARE(mouse.at(1).button, expectedButton);

        state.mouseEmulationTimer = 0;
        state.controller = nullptr;
    }
}

void InputIntegrationTest::legacyToggleOwnsReservedQuitWithCustomDeckChord()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    const QList<QList<Uint8>> pressOrders {
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_START},
        {SDL_CONTROLLER_BUTTON_START,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    };

    for (bool swapFaceButtons : {false, true}) {
        for (const auto& order : pressOrders) {
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            InputIntegrationStubs::reset();
            StreamingPreferences preferences(nullptr);
            initializePreferences(preferences);
            preferences.swapFaceButtons = swapFaceButtons;
            SdlInputHandler handler(preferences, 1920, 1080);
            GamepadState& state = handler.m_GamepadState[0];
            state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
            state.jsId = 92;
            state.index = 0;
            handler.m_GamepadMask = 1;
            DeckInputRouter router(DeckBindings(
                int(Qt::ControlModifier), SDL_SCANCODE_F8,
                customChord, false), swapFaceButtons);

            for (Uint8 button : order) {
                const auto result = routeThroughRealHandler(
                    router, handler,
                    controllerButtonEvent(
                        SDL_CONTROLLERBUTTONDOWN, state.jsId, button));
                QCOMPARE(result.disposition,
                         DeckInputRouter::Disposition::Passthrough);
            }
            for (Uint8 button : order) {
                QCOMPARE(routeThroughRealHandler(
                             router, handler,
                             controllerButtonEvent(
                                 SDL_CONTROLLERBUTTONUP,
                                 state.jsId, button)).disposition,
                         DeckInputRouter::Disposition::Passthrough);
            }
            QVERIFY(!InputIntegrationStubs::controllers().isEmpty());
            bool sawQuit = false;
            SDL_Event queued {};
            while (SDL_PollEvent(&queued)) {
                sawQuit |= queued.type == SDL_QUIT;
            }
            QVERIFY(!sawQuit);
            state.controller = nullptr;
        }
    }

    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    InputIntegrationStubs::reset();
    StreamingPreferences legacyPreferences(nullptr);
    initializePreferences(legacyPreferences);
    legacyPreferences.legacyGamepadDisconnect = true;
    SdlInputHandler legacyHandler(legacyPreferences, 1920, 1080);
    GamepadState& legacyState = legacyHandler.m_GamepadState[0];
    legacyState.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    legacyState.jsId = 93;
    legacyState.index = 0;
    legacyHandler.m_GamepadMask = 1;
    DeckInputRouter legacyRouter(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, true));
    for (Uint8 button : pressOrders.first()) {
        QCOMPARE(routeThroughRealHandler(
                     legacyRouter, legacyHandler,
                     controllerButtonEvent(
                         SDL_CONTROLLERBUTTONDOWN,
                         legacyState.jsId, button)).disposition,
                 DeckInputRouter::Disposition::Passthrough);
    }
    const auto legacyRecords = InputIntegrationStubs::controllers();
    QCOMPARE(legacyRecords.size(), 4);
    QCOMPARE(legacyRecords.last().buttons, 0);
    bool sawLegacyQuit = false;
    SDL_Event queued {};
    while (SDL_PollEvent(&queued)) {
        sawLegacyQuit |= queued.type == SDL_QUIT;
    }
    QVERIFY(sawLegacyQuit);
    legacyState.controller = nullptr;
}

void InputIntegrationTest::closedUnrelatedInputNeverUsesReservedQuitReplay()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 94;
    state.index = 0;
    handler.m_GamepadMask = 1;
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, false));

    QCOMPARE(routeThroughRealHandler(
                 router, handler,
                 controllerButtonEvent(
                     SDL_CONTROLLERBUTTONDOWN, state.jsId,
                     SDL_CONTROLLER_BUTTON_LEFTSHOULDER)).disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(InputIntegrationStubs::controllers().size(), 1);
    const auto second = routeThroughRealHandler(
        router, handler,
        controllerButtonEvent(
            SDL_CONTROLLERBUTTONDOWN, state.jsId,
            SDL_CONTROLLER_BUTTON_DPAD_UP));
    QCOMPARE(second.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(second.replayEvents.isEmpty());
    const auto records = InputIntegrationStubs::controllers();
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).buttons, LB_FLAG);
    QCOMPARE(records.at(1).buttons, int(LB_FLAG | UP_FLAG));
    state.controller = nullptr;
}

void InputIntegrationTest::localBackReplayDiscardsRemainingBufferAndReroutesCurrentToHost()
{
    for (bool swapFaceButtons : {false, true}) {
        InputIntegrationStubs::reset();
        StreamingPreferences preferences(nullptr);
        initializePreferences(preferences);
        preferences.swapFaceButtons = swapFaceButtons;
        SdlInputHandler handler(preferences, 1920, 1080);
        GamepadState& state = handler.m_GamepadState[0];
        state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
        state.jsId = 95;
        state.index = 0;
        handler.m_GamepadMask = 1;

        const Uint8 physicalBack = swapFaceButtons
            ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
        const Uint8 physicalActivate = swapFaceButtons
            ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
        const quint32 chord =
            (quint32(1) << physicalBack) |
            (quint32(1) << physicalActivate) |
            (quint32(1) << SDL_CONTROLLER_BUTTON_X);
        DeckInputRouter router(DeckBindings(
            int(Qt::ControlModifier), SDL_SCANCODE_F8,
            chord, false), swapFaceButtons);
        router.openForKeyboard();

        QCOMPARE(router.route(controllerButtonEvent(
                     SDL_CONTROLLERBUTTONDOWN, state.jsId,
                     physicalBack)).action,
                 DeckInputRouter::Action::None);
        QCOMPARE(router.route(controllerButtonEvent(
            SDL_CONTROLLERBUTTONDOWN, state.jsId,
            physicalActivate)).action,
            DeckInputRouter::Action::None);
        const auto aborted = router.route(controllerButtonEvent(
            SDL_CONTROLLERBUTTONDOWN, state.jsId,
            SDL_CONTROLLER_BUTTON_DPAD_UP));
        QCOMPARE(aborted.action, DeckInputRouter::Action::None);
        QCOMPARE(aborted.replayEvents.size(), 2);
        QVERIFY(aborted.deferredEvent.has_value());
        QCOMPARE(router.routeReplay(aborted.replayEvents.first()).action,
                 DeckInputRouter::Action::Back);

        // The real Deck closes while the buffered prefix is delivered.
        router.syncDeckOpen(false);
        const auto discarded = router.routeReplay(
            aborted.replayEvents.at(1));
        QCOMPARE(discarded.disposition,
                 DeckInputRouter::Disposition::Consumed);
        QCOMPARE(discarded.action, DeckInputRouter::Action::None);
        SDL_Event deferred = *aborted.deferredEvent;
        QCOMPARE(router.routeDeferred(deferred).disposition,
                 DeckInputRouter::Disposition::Passthrough);
        handler.handleControllerButtonEvent(&deferred.cbutton);
        QCOMPARE(InputIntegrationStubs::controllers().size(), 1);
        QCOMPARE(InputIntegrationStubs::controllers().first().buttons,
                 int(UP_FLAG));

        QCOMPARE(router.route(controllerButtonEvent(
                     SDL_CONTROLLERBUTTONUP, state.jsId,
                     physicalBack)).disposition,
                 DeckInputRouter::Disposition::Consumed);
        QCOMPARE(router.route(controllerButtonEvent(
                     SDL_CONTROLLERBUTTONUP, state.jsId,
                     physicalActivate)).disposition,
                 DeckInputRouter::Disposition::Consumed);
        SDL_Event currentUp = controllerButtonEvent(
            SDL_CONTROLLERBUTTONUP, state.jsId, physicalActivate);
        currentUp.cbutton.button = SDL_CONTROLLER_BUTTON_DPAD_UP;
        QCOMPARE(router.route(currentUp).disposition,
                 DeckInputRouter::Disposition::Passthrough);
        handler.handleControllerButtonEvent(&currentUp.cbutton);
        const auto records = InputIntegrationStubs::controllers();
        QCOMPARE(records.size(), 2);
        QCOMPARE(records.at(0).buttons, int(UP_FLAG));
        QCOMPARE(records.at(1).buttons, 0);
        state.controller = nullptr;
    }
}

void InputIntegrationTest::closedStartHoldReachesMouseModeTimingImmediately()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.gamepadMouse = true;
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 503;
    state.index = 0;
    handler.m_GamepadMask = 1;

    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, false));

    const auto startDown = routeThroughRealHandler(
        router, handler,
        controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN, state.jsId,
                              SDL_CONTROLLER_BUTTON_START));
    QCOMPARE(startDown.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(state.buttons, PLAY_FLAG);
    QCOMPARE(InputIntegrationStubs::controllers().size(), 1);

    state.lastStartDownTime = SDL_GetTicks() - 751;
    const auto startUp = routeThroughRealHandler(
        router, handler,
        controllerButtonEvent(SDL_CONTROLLERBUTTONUP, state.jsId,
                              SDL_CONTROLLER_BUTTON_START));
    QCOMPARE(startUp.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QVERIFY(state.mouseEmulationTimer != 0);
    QCOMPARE(InputIntegrationStubs::mouseEmulationNotifications(),
             QVector<bool>({true}));

    handler.cancelInputTimer(state.mouseEmulationTimer,
                             state.mouseEmulationTimerToken);
    state.controller = nullptr;
}

void InputIntegrationTest::closedConfiguredChordNeutralizesForwardedPrefixState()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 504;
    state.index = 0;
    handler.m_GamepadMask = 1;
    handler.m_RemoteInputState.controllerAllocated(0);

    const quint32 chord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8, chord, false));

    const auto prefix = routeThroughRealHandler(
        router, handler,
        controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN, state.jsId,
                              SDL_CONTROLLER_BUTTON_A));
    QCOMPARE(prefix.disposition,
             DeckInputRouter::Disposition::Passthrough);
    QCOMPARE(InputIntegrationStubs::controllers().size(), 1);
    QCOMPARE(InputIntegrationStubs::controllers().last().buttons, A_FLAG);

    const auto completed = routeThroughRealHandler(
        router, handler,
        controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN, state.jsId,
                              SDL_CONTROLLER_BUTTON_B));
    QCOMPARE(completed.action,
             DeckInputRouter::Action::OpenFromController);
    const auto records = InputIntegrationStubs::controllers();
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).buttons, A_FLAG);
    QCOMPARE(records.at(1).buttons, 0);
    QCOMPARE(state.buttons, 0);
    state.controller = nullptr;
}

void InputIntegrationTest::legacyOffIgnoresReservedQuitAfterUnrelatedButtonRelease()
{
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.legacyGamepadDisconnect = false;
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 505;
    state.index = 0;
    handler.m_GamepadMask = 1;

    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, false));

    for (SDL_GameControllerButton button : {
             SDL_CONTROLLER_BUTTON_DPAD_UP,
             SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
             SDL_CONTROLLER_BUTTON_BACK,
             SDL_CONTROLLER_BUTTON_START,
         }) {
        routeThroughRealHandler(
            router, handler,
            controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN,
                                  state.jsId, button));
    }
    routeThroughRealHandler(
        router, handler,
        controllerButtonEvent(SDL_CONTROLLERBUTTONUP, state.jsId,
                              SDL_CONTROLLER_BUTTON_DPAD_UP));

    bool sawQuit = false;
    SDL_Event queued {};
    while (SDL_PollEvent(&queued)) {
        sawQuit |= queued.type == SDL_QUIT;
    }
    QVERIFY(!sawQuit);
    QCOMPARE(state.buttons,
             int(PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG));
    state.controller = nullptr;
}

void InputIntegrationTest::legacyDisconnectHelperBypassesOverlayGate()
{
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.legacyGamepadDisconnect = true;
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 506;
    state.index = 0;
    state.buttons = PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG;
    handler.m_GamepadMask = 1;
    handler.beginLocalOverlayInput();

    QVERIFY(handler.handleLegacyGamepadDisconnect(state.jsId));
    bool sawQuit = false;
    SDL_Event queued {};
    while (SDL_PollEvent(&queued)) {
        sawQuit |= queued.type == SDL_QUIT;
    }
    QVERIFY(sawQuit);
    QCOMPARE(state.buttons, 0);
    state.controller = nullptr;
}

void InputIntegrationTest::legacyOnOpenDisconnectsThroughRealHandler()
{
    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    const QList<QList<Uint8>> pressOrders {
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_START},
        {SDL_CONTROLLER_BUTTON_START,
         SDL_CONTROLLER_BUTTON_BACK,
         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
         SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    };

    for (bool swapFaceButtons : {false, true}) {
        for (const auto& order : pressOrders) {
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            StreamingPreferences preferences(nullptr);
            initializePreferences(preferences);
            preferences.swapFaceButtons = swapFaceButtons;
            preferences.legacyGamepadDisconnect = true;
            SdlInputHandler handler(preferences, 1920, 1080);
            GamepadState& state = handler.m_GamepadState[0];
            state.controller =
                reinterpret_cast<SDL_GameController*>(quintptr(1));
            state.jsId = 507;
            state.index = 0;
            handler.m_GamepadMask = 1;
            DeckInputRouter router(DeckBindings(
                int(Qt::ControlModifier), SDL_SCANCODE_F8,
                customChord, true), swapFaceButtons);
            router.openForKeyboard();
            handler.beginLocalOverlayInput();

            DeckInputRouter::Result completed;
            for (Uint8 button : order) {
                completed = routeThroughRealHandler(
                    router, handler,
                    controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN,
                                          state.jsId, button));
            }
            QCOMPARE(completed.action,
                     DeckInputRouter::Action::LegacyDisconnect);
            bool sawQuit = false;
            SDL_Event queued {};
            while (SDL_PollEvent(&queued)) {
                sawQuit |= queued.type == SDL_QUIT;
            }
            QVERIFY(sawQuit);
            QCOMPARE(state.buttons, 0);
            for (Uint8 button : order) {
                QCOMPARE(routeThroughRealHandler(
                             router, handler,
                             controllerButtonEvent(SDL_CONTROLLERBUTTONUP,
                                                   state.jsId,
                                                   button)).disposition,
                         DeckInputRouter::Disposition::Consumed);
            }
            state.controller = nullptr;
        }
    }
}

void InputIntegrationTest::nonOwnerLegacyChordCannotQuitWhileDeckIsOwned()
{
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    preferences.legacyGamepadDisconnect = true;
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& owner = handler.m_GamepadState[0];
    owner.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    owner.jsId = 508;
    owner.index = 0;
    GamepadState& nonOwner = handler.m_GamepadState[1];
    nonOwner.controller = reinterpret_cast<SDL_GameController*>(quintptr(2));
    nonOwner.jsId = 509;
    nonOwner.index = 1;
    handler.m_GamepadMask = 0x3;

    const quint32 customChord =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_B);
    DeckInputRouter router(DeckBindings(
        int(Qt::ControlModifier), SDL_SCANCODE_F8,
        customChord, true));
    router.openForKeyboard();
    handler.beginLocalOverlayInput();

    QCOMPARE(routeThroughRealHandler(
                 router, handler,
                 controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN,
                                       owner.jsId,
                                       SDL_CONTROLLER_BUTTON_A)).action,
             DeckInputRouter::Action::Activate);
    QCOMPARE(router.controllerOwner(), owner.jsId);
    QCOMPARE(routeThroughRealHandler(
                 router, handler,
                 controllerButtonEvent(SDL_CONTROLLERBUTTONUP,
                                       owner.jsId,
                                       SDL_CONTROLLER_BUTTON_A)).disposition,
             DeckInputRouter::Disposition::Consumed);

    DeckInputRouter::Result completed;
    for (Uint8 button : {
             SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
             SDL_CONTROLLER_BUTTON_BACK,
             SDL_CONTROLLER_BUTTON_START,
         }) {
        completed = routeThroughRealHandler(
            router, handler,
            controllerButtonEvent(SDL_CONTROLLERBUTTONDOWN,
                                  nonOwner.jsId, button));
    }
    QCOMPARE(completed.disposition,
             DeckInputRouter::Disposition::Consumed);
    QCOMPARE(completed.action, DeckInputRouter::Action::None);
    bool sawQuit = false;
    SDL_Event queued {};
    while (SDL_PollEvent(&queued)) {
        sawQuit |= queued.type == SDL_QUIT;
    }
    QVERIFY(!sawQuit);
    QCOMPARE(router.controllerOwner(), owner.jsId);

    owner.controller = nullptr;
    nonOwner.controller = nullptr;
}

REGISTER_PERIGEE_TEST(InputIntegrationTest);

#include "test_inputintegration.moc"
