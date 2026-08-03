#include "test_registry.h"
#include "input_integration_stubs.h"

#include <QtTest>

#include <atomic>
#include <thread>

#define private public
#include "settings/streamingpreferences.h"
#include "streaming/input/input.h"
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

}

class InputIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void relativeTapReleaseTimerUsesTheOwningHandler();
    void nativeInputIsCancelledBeforeDeckOwnsInput();
    void ownershipWaitsForInFlightTimerSendBeforeNeutralizing();
    void gamepadMouseTimerCannotSendAfterOwnershipGate();
    void controllerAxisMutationWaitsForInFlightMouseCallback();
    void neutralRemoteInputPrecedesPhysicalCaptureRelease();
    void startupCaptureRequestsAreDeferredWhileDeckIsOpen();
    void statsChordNeutralizesOnlyItsController();
    void statsChordPreservesOtherPhysicalControllerInMergedMode();
    void statsChordRoutesBeforeTheRemoteHandlerAndConsumesReleaseTails();
    void routerRunsBeforeDeviceAndBatteryHousekeeping();
};

void InputIntegrationTest::init()
{
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

    QTRY_COMPARE_WITH_TIMEOUT(InputIntegrationStubs::mouseButtons().size(), 2, 1000);
    const auto records = InputIntegrationStubs::mouseButtons();
    QCOMPARE(records.at(0).action, int(BUTTON_ACTION_PRESS));
    QCOMPARE(records.at(0).button, BUTTON_LEFT);
    QCOMPARE(records.at(1).action, int(BUTTON_ACTION_RELEASE));
    QCOMPARE(records.at(1).button, BUTTON_LEFT);
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

void InputIntegrationTest::gamepadMouseTimerCannotSendAfterOwnershipGate()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState state {};
    state.inputHandler = &handler;
    state.lsX = 32767;

    InputIntegrationStubs::blockNextMouseMoveSend();
    std::atomic_bool ownershipReturned {false};
    std::thread timer([&state] {
        SdlInputHandler::mouseEmulationTimerCallback(50, &state);
    });
    QVERIFY(InputIntegrationStubs::waitUntilSendBlocked());
    std::thread owner([&handler, &ownershipReturned] {
        handler.beginLocalOverlayInput();
        ownershipReturned.store(true);
    });
    QTRY_VERIFY_WITH_TIMEOUT(handler.m_LocalOverlayInputActive.load(), 1000);
    QTest::qWait(20);
    QVERIFY(!ownershipReturned.load());
    InputIntegrationStubs::releaseBlockedSend();
    timer.join();
    owner.join();
    QVERIFY(ownershipReturned.load());
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 1);
    QCOMPARE(SdlInputHandler::mouseEmulationTimerCallback(50, &state), Uint32(50));
    QCOMPARE(InputIntegrationStubs::mouseMoveCount(), 1);
}

void InputIntegrationTest::controllerAxisMutationWaitsForInFlightMouseCallback()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    GamepadState& state = handler.m_GamepadState[0];
    state.inputHandler = &handler;
    state.controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    state.jsId = 44;
    state.index = 0;
    state.lsX = 32767;
    handler.m_GamepadMask = 1;

    InputIntegrationStubs::blockNextMouseMoveSend();
    std::atomic_bool axisReturned {false};
    std::thread timer([&state] {
        SdlInputHandler::mouseEmulationTimerCallback(50, &state);
    });
    QVERIFY(InputIntegrationStubs::waitUntilSendBlocked());
    SDL_ControllerAxisEvent axis {};
    axis.type = SDL_CONTROLLERAXISMOTION;
    axis.which = 44;
    axis.axis = SDL_CONTROLLER_AXIS_LEFTX;
    axis.value = 1234;
    std::thread mainMutation([&] {
        handler.handleControllerAxisEvent(&axis);
        axisReturned.store(true);
    });
    QTest::qWait(20);
    const bool returnedWhileCallbackWasInFlight = axisReturned.load();

    InputIntegrationStubs::releaseBlockedSend();
    timer.join();
    mainMutation.join();
    QVERIFY(!returnedWhileCallbackWasInFlight);
    QVERIFY(axisReturned.load());
    QCOMPARE(state.lsX, short(1234));
    state.controller = nullptr;
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

void InputIntegrationTest::statsChordRoutesBeforeTheRemoteHandlerAndConsumesReleaseTails()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_GamepadMask = 0x3;
    handler.m_GamepadState[0].controller = reinterpret_cast<SDL_GameController*>(quintptr(1));
    handler.m_GamepadState[0].jsId = 70;
    handler.m_GamepadState[0].index = 0;
    handler.m_GamepadState[1].controller = reinterpret_cast<SDL_GameController*>(quintptr(2));
    handler.m_GamepadState[1].jsId = 71;
    handler.m_GamepadState[1].index = 1;
    handler.m_GamepadState[1].buttons = A_FLAG;
    handler.m_RemoteInputState.keySent(12, true);
    handler.m_RemoteInputState.mouseButtonSent(BUTTON_RIGHT, true);
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
        if (result.disposition == DeckInputRouter::Disposition::Passthrough) {
            handler.handleControllerButtonEvent(&event.cbutton);
        }
        else {
            QCOMPARE(result.action, DeckInputRouter::Action::ToggleStats);
            QCOMPARE(result.controllerId, SDL_JoystickID(70));
            QVERIFY(handler.sendNeutralControllerInput(result.controllerId));
        }
    }

    QCOMPARE(handler.m_GamepadState[0].buttons, 0);
    QCOMPARE(handler.m_GamepadState[1].buttons, A_FLAG);
    QVERIFY(handler.m_RemoteInputState.hasKeysDown());
    QVERIFY(handler.m_RemoteInputState.hasMouseButtonsDown());
    const int recordsBeforeTails = InputIntegrationStubs::controllers().size();
    for (Uint8 button : chord) {
        SDL_Event event {};
        event.type = SDL_CONTROLLERBUTTONUP;
        event.cbutton.type = SDL_CONTROLLERBUTTONUP;
        event.cbutton.which = 70;
        event.cbutton.button = button;
        event.cbutton.state = SDL_RELEASED;
        QCOMPARE(router.route(event).disposition,
                 DeckInputRouter::Disposition::Consumed);
    }
    QCOMPARE(InputIntegrationStubs::controllers().size(), recordsBeforeTails);

    handler.m_GamepadState[0].controller = nullptr;
    handler.m_GamepadState[1].controller = nullptr;
}

void InputIntegrationTest::routerRunsBeforeDeviceAndBatteryHousekeeping()
{
    StreamingPreferences preferences(nullptr);
    initializePreferences(preferences);
    SdlInputHandler handler(preferences, 1920, 1080);
    handler.m_GamepadMask = 1;
    GamepadState& state = handler.m_GamepadState[0];
    state.inputHandler = &handler;
    state.jsId = 31;
    state.index = 0;
    handler.m_RemoteInputState.controllerAllocated(0);
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
}

REGISTER_PERIGEE_TEST(InputIntegrationTest);

#include "test_inputintegration.moc"
