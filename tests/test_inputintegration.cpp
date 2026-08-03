#include "test_registry.h"
#include "input_integration_stubs.h"

#include <QtTest>

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

StreamingPreferences::StreamingPreferences(QQmlEngine* engine)
    : m_QmlEngine(engine)
{
}

int StreamingPreferences::getDefaultBitrate(int, int, int, bool)
{
    return 0;
}

void StreamingPreferences::save()
{
}

bool StreamingPreferences::retranslate()
{
    return false;
}

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
    void gamepadTimerAddFailureDoesNotAdvertiseMouseMode();
    void staleTimerTokenIsIgnoredAfterHandlerReplacement();
    void neutralRemoteInputPrecedesPhysicalCaptureRelease();
    void startupCaptureRequestsAreDeferredWhileDeckIsOpen();
    void statsChordNeutralizesOnlyItsController();
    void statsChordPreservesOtherPhysicalControllerInMergedMode();
    void closedStatsChordUsesRemoteHandlerAndReleasesGamepadMouseButtons();
    void routerRunsBeforeDeviceAndBatteryHousekeeping();
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
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_GamepadMask = 0x1;
    handler.m_GamepadState[0].controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    handler.m_GamepadState[0].jsId = 70;
    handler.m_GamepadState[0].index = 0;
    handler.m_GamepadState[0].mouseEmulationTimer = SDL_TimerID(1);
    handler.sendTrackedMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
    DeckInputRouter router;
    const Uint8 chord[] = {
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_X,
    };

    for (Uint8 button : chord) {
        SDL_Event event {};
        event.type = SDL_CONTROLLERBUTTONDOWN;
        event.cbutton.type = SDL_CONTROLLERBUTTONDOWN;
        event.cbutton.which = 70;
        event.cbutton.button = button;
        event.cbutton.state = SDL_PRESSED;
        const auto result = router.route(event);
        QCOMPARE(result.disposition, DeckInputRouter::Disposition::Passthrough);
        handler.handleControllerButtonEvent(&event.cbutton);
    }

    for (Uint8 button : chord) {
        SDL_Event event {};
        event.type = SDL_CONTROLLERBUTTONUP;
        event.cbutton.type = SDL_CONTROLLERBUTTONUP;
        event.cbutton.which = 70;
        event.cbutton.button = button;
        event.cbutton.state = SDL_RELEASED;
        QCOMPARE(router.route(event).disposition,
                 DeckInputRouter::Disposition::Passthrough);
        handler.handleControllerButtonEvent(&event.cbutton);
    }

    const auto mouseButtons = InputIntegrationStubs::mouseButtons();
    QCOMPARE(mouseButtons.size(), 7);
    QCOMPARE(mouseButtons.at(0).button, BUTTON_RIGHT);
    QCOMPARE(mouseButtons.at(0).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(mouseButtons.at(1).button, BUTTON_X1);
    QCOMPARE(mouseButtons.at(1).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(mouseButtons.at(2).button, BUTTON_X2);
    QCOMPARE(mouseButtons.at(2).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(mouseButtons.at(3).button, BUTTON_MIDDLE);
    QCOMPARE(mouseButtons.at(3).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(mouseButtons.at(4).button, BUTTON_X1);
    QCOMPARE(mouseButtons.at(4).action, int(BUTTON_ACTION_RELEASE));
    QCOMPARE(mouseButtons.at(5).button, BUTTON_X2);
    QCOMPARE(mouseButtons.at(5).action, int(BUTTON_ACTION_RELEASE));
    QCOMPARE(mouseButtons.at(6).button, BUTTON_MIDDLE);
    QCOMPARE(mouseButtons.at(6).action, int(BUTTON_ACTION_RELEASE));
    QVERIFY(handler.m_RemoteInputState.hasMouseButtonsDown());

    handler.m_GamepadState[0].mouseEmulationTimer = 0;
    handler.m_GamepadState[0].controller = nullptr;
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

REGISTER_PERIGEE_TEST(InputIntegrationTest);

#include "test_inputintegration.moc"
