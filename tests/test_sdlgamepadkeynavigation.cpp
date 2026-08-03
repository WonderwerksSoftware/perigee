#include "test_registry.h"

#define private public
#include "gui/sdlgamepadkeynavigation.h"
#include "settings/streamingpreferences.h"
#undef private

#include <QSignalSpy>
#include <QtTest>

namespace {

SDL_Event buttonEvent(Uint32 type, SDL_GameControllerButton button,
                      SDL_JoystickID controller = 17)
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

}

class SdlGamepadKeyNavigationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void rawControllerCaptureSuppressesNavigationAndIgnoresFaceSwap();
    void captureRemainsOwnedByFirstController();
    void aSingleBackButtonCancelsRawControllerCapture();
    void focusLossCancelsCaptureAndAllowsANewController();
    void disableCancelsCaptureEvenBeforeNavigationWasEnabled();
    void ownerRemovalCancelsCaptureAndReconnectCanRecover();
    void analogNavigationIsGatedWhileCapturing();
};

void SdlGamepadKeyNavigationTest::initTestCase()
{
    QVERIFY(SDL_InitSubSystem(SDL_INIT_EVENTS) == 0);
}

void SdlGamepadKeyNavigationTest::init()
{
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

void SdlGamepadKeyNavigationTest::rawControllerCaptureSuppressesNavigationAndIgnoresFaceSwap()
{
    StreamingPreferences preferences(nullptr);
    preferences.swapFaceButtons = true;
    SdlGamepadKeyNavigation navigation(&preferences);
    QSignalSpy captured(&navigation,
                        &SdlGamepadKeyNavigation::controllerBindingCaptured);
    navigation.beginControllerBindingCapture();

    for (SDL_Event event : {
             buttonEvent(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A),
             buttonEvent(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_Y),
             buttonEvent(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A),
             buttonEvent(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_Y),
         }) {
        QCOMPARE(SDL_PushEvent(&event), 1);
        navigation.onPollingTimerFired();
    }

    QCOMPARE(captured.size(), 1);
    const quint32 expected =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_Y);
    QCOMPARE(captured.first().first().toUInt(), expected);
    QVERIFY(!navigation.m_DeckBindingCapture.isActive());
}

void SdlGamepadKeyNavigationTest::captureRemainsOwnedByFirstController()
{
    StreamingPreferences preferences(nullptr);
    SdlGamepadKeyNavigation navigation(&preferences);
    QSignalSpy captured(&navigation,
                        &SdlGamepadKeyNavigation::controllerBindingCaptured);
    navigation.beginControllerBindingCapture();

    navigation.handleControllerBindingButton(
        17, SDL_CONTROLLER_BUTTON_A, true);
    navigation.handleControllerBindingButton(
        18, SDL_CONTROLLER_BUTTON_X, true);
    navigation.handleControllerBindingButton(
        18, SDL_CONTROLLER_BUTTON_X, false);
    navigation.handleControllerBindingButton(
        17, SDL_CONTROLLER_BUTTON_Y, true);
    navigation.handleControllerBindingButton(
        17, SDL_CONTROLLER_BUTTON_A, false);
    navigation.handleControllerBindingButton(
        17, SDL_CONTROLLER_BUTTON_Y, false);

    QCOMPARE(captured.size(), 1);
    const quint32 expected =
        (quint32(1) << SDL_CONTROLLER_BUTTON_A) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_Y);
    QCOMPARE(captured.first().first().toUInt(), expected);
}

void SdlGamepadKeyNavigationTest::aSingleBackButtonCancelsRawControllerCapture()
{
    StreamingPreferences preferences(nullptr);
    SdlGamepadKeyNavigation navigation(&preferences);
    QSignalSpy captured(&navigation,
                        &SdlGamepadKeyNavigation::controllerBindingCaptured);
    QSignalSpy cancelled(&navigation,
                         &SdlGamepadKeyNavigation::controllerBindingCaptureCancelled);
    navigation.beginControllerBindingCapture();

    for (SDL_Event event : {
             buttonEvent(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_B),
             buttonEvent(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_B),
         }) {
        QCOMPARE(SDL_PushEvent(&event), 1);
    }

    navigation.onPollingTimerFired();

    QCOMPARE(captured.size(), 0);
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!navigation.m_DeckBindingCapture.isActive());
}

void SdlGamepadKeyNavigationTest::focusLossCancelsCaptureAndAllowsANewController()
{
    StreamingPreferences preferences(nullptr);
    SdlGamepadKeyNavigation navigation(&preferences);
    QSignalSpy cancelled(&navigation,
                         &SdlGamepadKeyNavigation::controllerBindingCaptureCancelled);
    QSignalSpy captured(&navigation,
                        &SdlGamepadKeyNavigation::controllerBindingCaptured);
    navigation.beginControllerBindingCapture();
    navigation.handleControllerBindingButton(17, SDL_CONTROLLER_BUTTON_A, true);

    navigation.notifyWindowFocus(false);

    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!navigation.m_DeckBindingCapture.isActive());
    navigation.beginControllerBindingCapture();
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_X, true);
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_Y, true);
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_X, false);
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_Y, false);
    QCOMPARE(captured.size(), 1);
}

void SdlGamepadKeyNavigationTest::disableCancelsCaptureEvenBeforeNavigationWasEnabled()
{
    StreamingPreferences preferences(nullptr);
    SdlGamepadKeyNavigation navigation(&preferences);
    QSignalSpy cancelled(&navigation,
                         &SdlGamepadKeyNavigation::controllerBindingCaptureCancelled);
    navigation.beginControllerBindingCapture();

    navigation.disable();

    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!navigation.m_DeckBindingCapture.isActive());
}

void SdlGamepadKeyNavigationTest::ownerRemovalCancelsCaptureAndReconnectCanRecover()
{
    StreamingPreferences preferences(nullptr);
    SdlGamepadKeyNavigation navigation(&preferences);
    QSignalSpy cancelled(&navigation,
                         &SdlGamepadKeyNavigation::controllerBindingCaptureCancelled);
    QSignalSpy captured(&navigation,
                        &SdlGamepadKeyNavigation::controllerBindingCaptured);
    navigation.beginControllerBindingCapture();
    navigation.handleControllerBindingButton(17, SDL_CONTROLLER_BUTTON_A, true);

    navigation.handleControllerDeviceRemoved(17);

    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!navigation.m_DeckBindingCapture.isActive());
    navigation.beginControllerBindingCapture();
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_A, true);
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_Y, true);
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_A, false);
    navigation.handleControllerBindingButton(18, SDL_CONTROLLER_BUTTON_Y, false);
    QCOMPARE(captured.size(), 1);
}

void SdlGamepadKeyNavigationTest::analogNavigationIsGatedWhileCapturing()
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const int deviceIndex = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX,
        SDL_CONTROLLER_BUTTON_MAX,
        0);
    QVERIFY2(deviceIndex >= 0, SDL_GetError());

    StreamingPreferences preferences(nullptr);
    SdlGamepadKeyNavigation navigation(&preferences);
    navigation.enable();
    QVERIFY(!navigation.m_Gamepads.isEmpty());
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(
        navigation.m_Gamepads.first());
    QVERIFY(joystick != nullptr);
    QCOMPARE(SDL_JoystickSetVirtualAxis(
                 joystick, SDL_CONTROLLER_AXIS_LEFTX, 32767), 0);
    SDL_JoystickUpdate();
    navigation.m_FirstPoll = false;
    navigation.m_LastAxisNavigationEventTime = SDL_GetTicks() - 200;
    const Uint32 before = navigation.m_LastAxisNavigationEventTime;
    navigation.beginControllerBindingCapture();

    navigation.onPollingTimerFired();

    QCOMPARE(navigation.m_LastAxisNavigationEventTime, before);
    navigation.disable();
    if (SDL_JoystickIsVirtual(deviceIndex)) {
        QCOMPARE(SDL_JoystickDetachVirtual(deviceIndex), 0);
    }
#else
    QSKIP("SDL virtual controllers require SDL 2.0.14 or newer");
#endif
}

REGISTER_PERIGEE_TEST(SdlGamepadKeyNavigationTest);

#include "test_sdlgamepadkeynavigation.moc"
