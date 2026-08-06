#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/session.h"
#include "streaming/sdleventcodes.h"
#include "settings/mappingmanager.h"
#include "path.h"
#include "utils.h"

#include <QtGlobal>
#include <QDir>
#include <QGuiApplication>
#include <QtMath>

#include <atomic>
#include <cstdlib>

namespace {

std::atomic_uint32_t s_NextInputTimerToken {1};

uint32_t nextInputTimerToken()
{
    uint32_t token;
    do {
        token = s_NextInputTimerToken.fetch_add(1, std::memory_order_relaxed);
    } while (token == 0);
    return token;
}

}

SdlInputHandler::SdlInputHandler(StreamingPreferences& prefs, int streamWidth, int streamHeight)
    : m_Window(nullptr),
      m_MultiController(prefs.multiController),
      m_GamepadMouse(prefs.gamepadMouse),
      m_SwapMouseButtons(prefs.swapMouseButtons),
      m_ReverseScrollDirection(prefs.reverseScrollDirection),
      m_SwapFaceButtons(prefs.swapFaceButtons),
      m_LegacyGamepadDisconnect(prefs.legacyGamepadDisconnect),
      m_MouseWasInVideoRegion(false),
      m_PendingMouseButtonsAllUpOnVideoRegionLeave(false),
      m_PointerRegionLockActive(false),
      m_PointerRegionLockToggledByUser(false),
      m_FakeMouseCaptureActive(false),
      m_KeyboardCaptureActive(false),
      m_CaptureSystemKeysMode(prefs.captureSysKeysMode),
      m_MouseCursorCapturedVisibilityState(SDL_DISABLE),
      m_LongPressTimer(0),
      m_LongPressTimerToken(0),
      m_StreamWidth(streamWidth),
      m_StreamHeight(streamHeight),
      m_AbsoluteMouseMode(prefs.absoluteMouseMode),
      m_AbsoluteTouchMode(prefs.absoluteTouchMode),
      m_DisabledTouchFeedback(false),
      m_LeftButtonReleaseTimer(0),
      m_LeftButtonReleaseTimerToken(0),
      m_RightButtonReleaseTimer(0),
      m_RightButtonReleaseTimerToken(0),
      m_DragTimer(0),
      m_DragTimerToken(0),
      m_DragButton(0),
      m_NumFingersDown(0)
{
    // System keys are always captured when running without a DE
    if (!WMUtils::isRunningDesktopEnvironment()) {
        m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
    }

    // SDL3 breaks our auto-capture-on-leave logic because the mouse focus has already
    // been lost by the time we attempt to call SDL_CaptureMouse(). Fortunately, SDL3's
    // own auto-capture logic seems to be stable now (unlike SDL2), so we can rely on
    // that instead of our own hack when running on sdl2-compat.
    // https://github.com/libsdl-org/SDL/commit/e54001b02809dcebbb822bd0297919c8c76976a1
    SDL_version ver;
    SDL_GetVersion(&ver);
    m_NeedsManualCaptureOnLeave = !(ver.major == 2 && ver.minor >= 30 && ver.patch >= 50) && !SDL_GetHint("SDL3_VERSION");
    if (m_NeedsManualCaptureOnLeave) {
        // Disable the buggy auto-capture on earlier SDL2 builds
        SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
    }

    // Allow gamepad input when the app doesn't have focus if requested
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, prefs.backgroundGamepad ? "1" : "0");

#if !SDL_VERSION_ATLEAST(2, 0, 15)
    // For older versions of SDL (2.0.14 and earlier), use SDL_HINT_GRAB_KEYBOARD
    SDL_SetHintWithPriority(SDL_HINT_GRAB_KEYBOARD,
                            m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF ? "1" : "0",
                            SDL_HINT_OVERRIDE);
#endif

    // Opt-out of SDL's built-in Alt+Tab handling while keyboard grab is enabled
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");

    // Allow clicks to pass through to us when focusing the window. If we're in
    // absolute mouse mode, this will avoid the user having to click twice to
    // trigger a click on the host if the Moonlight window is not focused. In
    // relative mode, the click event will trigger the mouse to be recaptured.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // Enabling extended input reports allows rumble to function on Bluetooth PS4/PS5
    // controllers, but breaks DirectInput applications. We will enable it because
    // it's likely that working rumble is what the user is expecting. If they don't
    // want this behavior, they can override it with the environment variable.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");

    // Populate special key combo configuration
    m_SpecialKeyCombos[KeyComboQuit].keyCombo = KeyComboQuit;
    m_SpecialKeyCombos[KeyComboQuit].keyCode = SDLK_q;
    m_SpecialKeyCombos[KeyComboQuit].scanCode = SDL_SCANCODE_Q;
    m_SpecialKeyCombos[KeyComboQuit].enabled = true;

    m_SpecialKeyCombos[KeyComboUngrabInput].keyCombo = KeyComboUngrabInput;
    m_SpecialKeyCombos[KeyComboUngrabInput].keyCode = SDLK_z;
    m_SpecialKeyCombos[KeyComboUngrabInput].scanCode = SDL_SCANCODE_Z;
    m_SpecialKeyCombos[KeyComboUngrabInput].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCombo = KeyComboToggleFullScreen;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCode = SDLK_x;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].scanCode = SDL_SCANCODE_X;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCombo = KeyComboToggleStatsOverlay;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCode = SDLK_s;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].scanCode = SDL_SCANCODE_S;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleMouseMode].keyCombo = KeyComboToggleMouseMode;
    m_SpecialKeyCombos[KeyComboToggleMouseMode].keyCode = SDLK_m;
    m_SpecialKeyCombos[KeyComboToggleMouseMode].scanCode = SDL_SCANCODE_M;
    m_SpecialKeyCombos[KeyComboToggleMouseMode].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleCursorHide].keyCombo = KeyComboToggleCursorHide;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].keyCode = SDLK_c;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].scanCode = SDL_SCANCODE_C;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCombo = KeyComboToggleMinimize;
    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCode = SDLK_d;
    m_SpecialKeyCombos[KeyComboToggleMinimize].scanCode = SDL_SCANCODE_D;
    m_SpecialKeyCombos[KeyComboToggleMinimize].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboPasteText].keyCombo = KeyComboPasteText;
    m_SpecialKeyCombos[KeyComboPasteText].keyCode = SDLK_v;
    m_SpecialKeyCombos[KeyComboPasteText].scanCode = SDL_SCANCODE_V;
    m_SpecialKeyCombos[KeyComboPasteText].enabled = true;

    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCombo = KeyComboTogglePointerRegionLock;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCode = SDLK_l;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].scanCode = SDL_SCANCODE_L;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].enabled = true;

    m_SpecialKeyCombos[KeyComboQuitAndExit].keyCombo = KeyComboQuitAndExit;
    m_SpecialKeyCombos[KeyComboQuitAndExit].keyCode = SDLK_e;
    m_SpecialKeyCombos[KeyComboQuitAndExit].scanCode = SDL_SCANCODE_E;
    m_SpecialKeyCombos[KeyComboQuitAndExit].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].keyCombo = KeyComboToggleKeyboardGrab;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].keyCode = SDLK_k;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].scanCode = SDL_SCANCODE_K;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].enabled = WMUtils::isRunningDesktopEnvironment();

    m_OldIgnoreDevices = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES);
    m_OldIgnoreDevicesExcept = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);

    QString streamIgnoreDevices = qgetenv("STREAM_GAMECONTROLLER_IGNORE_DEVICES");
    QString streamIgnoreDevicesExcept = qgetenv("STREAM_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT");

    if (!streamIgnoreDevices.isEmpty() && !streamIgnoreDevices.endsWith(',')) {
        streamIgnoreDevices += ',';
    }
    streamIgnoreDevices += m_OldIgnoreDevices;

    // STREAM_IGNORE_DEVICE_GUIDS allows to specify additional devices to be ignored when starting
    // the stream in case the scope of STREAM_GAMECONTROLLER_IGNORE_DEVICES is too broad. One such
    // case is "Steam Virtual Gamepad" where everything is under the same VID/PID, but different GUIDs.
    // Multiple GUIDs can be provided, but need to be separated by commas:
    //
    //     <GUID>,<GUID>,<GUID>,...
    //
    QString streamIgnoreDeviceGuids = qgetenv("STREAM_IGNORE_DEVICE_GUIDS");
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    m_IgnoreDeviceGuids = streamIgnoreDeviceGuids.split(',', Qt::SkipEmptyParts);
#else
    m_IgnoreDeviceGuids = streamIgnoreDeviceGuids.split(',', QString::SkipEmptyParts);
#endif

    // For SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, we use the union of SDL_GAMECONTROLLER_IGNORE_DEVICES
    // and STREAM_GAMECONTROLLER_IGNORE_DEVICES while streaming. STREAM_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT
    // overrides SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT while streaming.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, streamIgnoreDevices.toUtf8());
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, streamIgnoreDevicesExcept.toUtf8());

    // We must initialize joystick explicitly before gamecontroller in order
    // to ensure we receive gamecontroller attach events for gamepads where
    // SDL doesn't have a built-in mapping. By starting joystick first, we
    // can allow mapping manager to update the mappings before GC attach
    // events are generated.
    SDL_assert(!SDL_WasInit(SDL_INIT_JOYSTICK));
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_JOYSTICK) failed: %s",
                     SDL_GetError());
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Flush gamepad arrival and departure events which may be queued before
    // starting the gamecontroller subsystem again. This prevents us from
    // receiving duplicate arrival and departure events for the same gamepad.
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);
    SDL_FlushEvent(SDL_CONTROLLERDEVICEREMOVED);

    // We need to reinit this each time, since you only get
    // an initial set of gamepad arrival events once per init.
    SDL_assert(!SDL_WasInit(SDL_INIT_GAMECONTROLLER));
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
    }

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_assert(!SDL_WasInit(SDL_INIT_HAPTIC));
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_HAPTIC) failed: %s",
                     SDL_GetError());
    }
#endif

    // Initialize the gamepad mask with currently attached gamepads to avoid
    // causing gamepads to unexpectedly disappear and reappear on the host
    // during stream startup as we detect currently attached gamepads one at a time.
    m_GamepadMask = getAttachedGamepadMask();

    SDL_zero(m_GamepadState);
    SDL_zero(m_LastTouchDownEvent);
    SDL_zero(m_LastTouchUpEvent);
    SDL_zero(m_TouchDownEvent);
}

SdlInputHandler::~SdlInputHandler()
{
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].mouseEmulationTimer != 0) {
            Session::get()->notifyMouseEmulationMode(false);
            cancelInputTimer(m_GamepadState[i].mouseEmulationTimer,
                             m_GamepadState[i].mouseEmulationTimerToken);
        }
#if !SDL_VERSION_ATLEAST(2, 0, 9)
        if (m_GamepadState[i].haptic != nullptr) {
            SDL_HapticClose(m_GamepadState[i].haptic);
        }
#endif
        if (m_GamepadState[i].controller != nullptr) {
            SDL_GameControllerClose(m_GamepadState[i].controller);
        }
    }

    cancelInputTimer(m_LongPressTimer, m_LongPressTimerToken);
    cancelInputTimer(m_LeftButtonReleaseTimer, m_LeftButtonReleaseTimerToken);
    cancelInputTimer(m_RightButtonReleaseTimer, m_RightButtonReleaseTimerToken);
    cancelInputTimer(m_DragTimer, m_DragTimerToken);
    m_InputTimerRequests.clear();

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    SDL_assert(!SDL_WasInit(SDL_INIT_HAPTIC));
#endif

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_assert(!SDL_WasInit(SDL_INIT_GAMECONTROLLER));

    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    SDL_assert(!SDL_WasInit(SDL_INIT_JOYSTICK));

    // Return background event handling to off
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");

    // Restore the ignored devices
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, m_OldIgnoreDevices.toUtf8());
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, m_OldIgnoreDevicesExcept.toUtf8());

#ifdef STEAM_LINK
    // Hide SDL's cursor on Steam Link after quitting the stream.
    // FIXME: We should also do this for other situations where SDL
    // and Qt will draw their own mouse cursors like KMSDRM or RPi
    // video backends.
    SDL_ShowCursor(SDL_DISABLE);
#endif
}

void SdlInputHandler::setWindow(SDL_Window *window)
{
    m_Window = window;
}

bool SdlInputHandler::startInputTimer(SDL_TimerID& timer, uint32_t& token,
                                      Uint32 interval, InputTimerAction action,
                                      SDL_JoystickID controllerId,
                                      bool repeating)
{
    cancelInputTimer(timer, token);
    do {
        token = nextInputTimerToken();
    } while (m_InputTimerRequests.contains(token));
    m_InputTimerRequests.insert(token, {action, controllerId, repeating});
    const SDL_TimerCallback callback = repeating
        ? mouseEmulationTimerCallback : longPressTimerCallback;
    timer = SDL_AddTimer(interval, callback,
                         reinterpret_cast<void*>(uintptr_t(token)));
    if (timer == 0) {
        m_InputTimerRequests.remove(token);
        token = 0;
    }
    return timer != 0;
}

void SdlInputHandler::cancelInputTimer(SDL_TimerID& timer, uint32_t& token)
{
    if (token != 0) {
        m_InputTimerRequests.remove(token);
        token = 0;
    }
    if (timer != 0) {
        const SDL_TimerID cancelledTimer = timer;
        timer = 0;
        SDL_RemoveTimer(cancelledTimer);
    }
}

bool SdlInputHandler::pushInputTimerEvent(void* param)
{
    SDL_Event event {};
    event.type = SDL_USEREVENT;
    event.user.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_INPUT_TIMER;
    event.user.data1 = param;
    return SDL_PushEvent(&event) == 1;
}

bool SdlInputHandler::handleInputTimerEvent(const SDL_UserEvent& event)
{
    if (event.type != SDL_USEREVENT || event.code != SDL_CODE_INPUT_TIMER) {
        return false;
    }

    const uint32_t token = uint32_t(uintptr_t(event.data1));
    const auto it = m_InputTimerRequests.constFind(token);
    if (it == m_InputTimerRequests.cend()) {
        return true;
    }
    const InputTimerRequest request = *it;
    if (!request.repeating) {
        m_InputTimerRequests.remove(token);
        if (m_LongPressTimerToken == token) {
            m_LongPressTimer = 0;
            m_LongPressTimerToken = 0;
        }
        else if (m_LeftButtonReleaseTimerToken == token) {
            m_LeftButtonReleaseTimer = 0;
            m_LeftButtonReleaseTimerToken = 0;
        }
        else if (m_RightButtonReleaseTimerToken == token) {
            m_RightButtonReleaseTimer = 0;
            m_RightButtonReleaseTimerToken = 0;
        }
        else if (m_DragTimerToken == token) {
            m_DragTimer = 0;
            m_DragTimerToken = 0;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    if (!m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        dispatchInputTimer(request);
    }
    return true;
}

void SdlInputHandler::dispatchInputTimer(const InputTimerRequest& request)
{
    switch (request.action) {
    case InputTimerAction::LongPress:
        sendTrackedMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        sendTrackedMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        break;
    case InputTimerAction::ReleaseLeftButton:
        sendTrackedMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        break;
    case InputTimerAction::ReleaseRightButton:
        sendTrackedMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
        break;
    case InputTimerAction::Drag:
        if (m_NumFingersDown == 2) {
            m_DragButton = BUTTON_RIGHT;
        }
        else if (m_NumFingersDown == 1) {
            m_DragButton = BUTTON_LEFT;
        }
        if (m_DragButton != 0) {
            sendTrackedMouseButtonEvent(BUTTON_ACTION_PRESS, m_DragButton);
        }
        break;
    case InputTimerAction::MouseEmulation: {
        GamepadState* gamepad = findStateForGamepad(request.controllerId);
        if (gamepad == nullptr || gamepad->mouseEmulationTimer == 0) {
            break;
        }
        int rawX;
        int rawY;
        if (std::abs(int(gamepad->lsX)) + std::abs(int(gamepad->lsY)) >
                std::abs(int(gamepad->rsX)) + std::abs(int(gamepad->rsY))) {
            rawX = gamepad->lsX;
            rawY = -gamepad->lsY;
        }
        else {
            rawX = gamepad->rsX;
            rawY = -gamepad->rsY;
        }
        float deltaX = qPow(rawX / 32766.0f *
                                MouseEmulationMotionMultiplier, 3);
        float deltaY = qPow(rawY / 32766.0f *
                                MouseEmulationMotionMultiplier, 3);
        deltaX = qAbs(deltaX) > MouseEmulationDeadzone
            ? deltaX - MouseEmulationDeadzone : 0;
        deltaY = qAbs(deltaY) > MouseEmulationDeadzone
            ? deltaY - MouseEmulationDeadzone : 0;
        if (deltaX != 0 || deltaY != 0) {
            LiSendMouseMoveEvent(short(deltaX), short(deltaY));
        }
        break;
    }
    }
}

void SdlInputHandler::raiseAllKeys()
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    const QVector<short> keys = m_RemoteInputState.takeKeyReleases();
    if (keys.isEmpty()) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Raising %d keys",
                (int)keys.count());

    for (short keyDown : keys) {
        LiSendKeyboardEvent(keyDown, KEY_ACTION_UP, 0);
    }
}

CaptureSnapshot SdlInputHandler::beginLocalOverlayInput()
{
    const CaptureSnapshot snapshot {
        isCaptureActive(),
        keyboardCaptureEnabled(),
    };
    m_DeferredCaptureActive.reset();
    m_LocalOverlayInputActive.store(true, std::memory_order_release);

    // Rotate or erase every timer token at the ownership boundary. Any timer
    // event already queued with an old token then becomes harmless.
    cancelInputTimer(m_LongPressTimer, m_LongPressTimerToken);
    cancelInputTimer(m_LeftButtonReleaseTimer, m_LeftButtonReleaseTimerToken);
    cancelInputTimer(m_RightButtonReleaseTimer, m_RightButtonReleaseTimerToken);
    cancelInputTimer(m_DragTimer, m_DragTimerToken);
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        GamepadState& state = m_GamepadState[i];
        if (state.mouseEmulationTimer != 0) {
            const SDL_JoystickID controllerId = state.jsId;
            cancelInputTimer(state.mouseEmulationTimer,
                             state.mouseEmulationTimerToken);
            if (!startInputTimer(state.mouseEmulationTimer,
                                 state.mouseEmulationTimerToken,
                                 MouseEmulationPollingInterval,
                                 InputTimerAction::MouseEmulation,
                                 controllerId,
                                 true)) {
                Session::get()->notifyMouseEmulationMode(false);
            }
        }
    }
    m_DragButton = 0;
    m_NumFingersDown = 0;
    SDL_zero(m_LastTouchDownEvent);
    SDL_zero(m_LastTouchUpEvent);
    SDL_zero(m_TouchDownEvent);

    // Timer callbacks only enqueue opaque tokens. All input-state reads and
    // remote sends happen below on the SDL event thread.
    sendNeutralRemoteInput();
    applyCaptureActive(false);
    return snapshot;
}

void SdlInputHandler::endLocalOverlayInput(CaptureSnapshot snapshot,
                                           bool keepReleased)
{
    sendNeutralRemoteInput();
    const bool restoreCapture = m_DeferredCaptureActive.value_or(snapshot.mouseCaptured);
    m_DeferredCaptureActive.reset();
    applyCaptureActive(!keepReleased && restoreCapture);
    if (!keepReleased && snapshot.keyboardCaptured &&
            !m_KeyboardCaptureActive) {
        updateKeyboardGrabState();
    }
    // Invalidate mouse events produced while Deck owned input before reopening
    // the remote-input gate. A fresh timer keeps an already-active mode active.
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        GamepadState& state = m_GamepadState[i];
        if (state.mouseEmulationTimer != 0) {
            const SDL_JoystickID controllerId = state.jsId;
            cancelInputTimer(state.mouseEmulationTimer,
                             state.mouseEmulationTimerToken);
            if (!startInputTimer(state.mouseEmulationTimer,
                                 state.mouseEmulationTimerToken,
                                 MouseEmulationPollingInterval,
                                 InputTimerAction::MouseEmulation,
                                 controllerId,
                                 true)) {
                Session::get()->notifyMouseEmulationMode(false);
            }
        }
    }
    m_LocalOverlayInputActive.store(false, std::memory_order_release);
}

void SdlInputHandler::sendNeutralRemoteInput()
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    const NeutralRemoteInput neutral = m_RemoteInputState.takeNeutralInput();
    if (neutral.cancelAllTouches) {
        LiSendTouchEvent(LI_TOUCH_EVENT_CANCEL_ALL, 0, 0, 0, 0,
                         0, 0, LI_ROT_UNKNOWN);
    }
    if (neutral.cancelPen) {
        LiSendPenEvent(LI_TOUCH_EVENT_CANCEL, LI_TOOL_TYPE_PEN, 0,
                       0, 0, 0, 0, 0, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
    }
    for (short keyCode : neutral.keyReleases) {
        LiSendKeyboardEvent(keyCode, KEY_ACTION_UP, 0);
    }
    for (int button : neutral.mouseButtonReleases) {
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
    }

    for (GamepadState& state : m_GamepadState) {
        if (state.controller == nullptr) {
            continue;
        }
        state.buttons = 0;
        state.lsX = state.lsY = 0;
        state.rsX = state.rsY = 0;
        state.lt = state.rt = 0;
        state.emulatedClickpadButtonDown = false;
    }
    for (short controllerIndex : neutral.controllerIndicesToZero) {
        LiSendMultiControllerEvent(controllerIndex, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
    }
}

bool SdlInputHandler::sendNeutralControllerInput(SDL_JoystickID id)
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    GamepadState* state = findStateForGamepad(id);
    if (state == nullptr) {
        return false;
    }

    state->buttons = 0;
    state->lsX = state->lsY = 0;
    state->rsX = state->rsY = 0;
    state->lt = state->rt = 0;
    state->emulatedClickpadButtonDown = false;
    if (m_MultiController) {
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
    }
    else {
        sendGamepadState(state);
    }
    return true;
}

bool SdlInputHandler::sendPhysicalDisplayShortcut(int displayNumber)
{
    if (displayNumber < 1 || displayNumber > 13) {
        return false;
    }

    struct KeyboardPacket {
        short keyCode;
        char action;
    };

    const short functionKey = static_cast<short>(0x8070 + displayNumber - 1);
    const KeyboardPacket packets[] {
        {static_cast<short>(0x8011), KEY_ACTION_DOWN},
        {static_cast<short>(0x8012), KEY_ACTION_DOWN},
        {static_cast<short>(0x8010), KEY_ACTION_DOWN},
        {functionKey, KEY_ACTION_DOWN},
        {functionKey, KEY_ACTION_UP},
        {static_cast<short>(0x8010), KEY_ACTION_UP},
        {static_cast<short>(0x8012), KEY_ACTION_UP},
        {static_cast<short>(0x8011), KEY_ACTION_UP},
    };

    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    bool sent = true;
    for (const KeyboardPacket& packet : packets) {
        if (LiSendKeyboardEvent2(packet.keyCode, packet.action, 0, 0) != 0) {
            sent = false;
        }
    }
    return sent;
}

void SdlInputHandler::sendTrackedMouseButtonEvent(int action, int button)
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    if (m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        return;
    }
    const bool pressed = action == BUTTON_ACTION_PRESS;
    m_RemoteInputState.mouseButtonSent(button, pressed);
    LiSendMouseButtonEvent(action, button);
}

void SdlInputHandler::sendTrackedKeyboardEvent(short keyCode, char action,
                                               char modifiers, char flags)
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    if (m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        return;
    }
    m_RemoteInputState.keySent(keyCode, action == KEY_ACTION_DOWN);
    LiSendKeyboardEvent2(0x8000 | keyCode, action, modifiers, flags);
}

void SdlInputHandler::sendTrackedTouchEvent(uint8_t eventType,
                                            uint32_t pointerId,
                                            float x, float y,
                                            float pressure)
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    if (m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        return;
    }
    m_RemoteInputState.touchSent(eventType, pointerId);
    LiSendTouchEvent(eventType, pointerId, x, y, pressure,
                     0, 0, LI_ROT_UNKNOWN);
}

void SdlInputHandler::sendTrackedPenEvent(uint8_t eventType, float x, float y,
                                          float pressure)
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    if (m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        return;
    }
    m_RemoteInputState.penSent(eventType);
    LiSendPenEvent(eventType, LI_TOOL_TYPE_PEN, 0, x, y, pressure,
                   0, 0, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
}

void SdlInputHandler::notifyMouseLeave()
{
    if (m_NeedsManualCaptureOnLeave) {
        // SDL on Windows doesn't send the mouse button up until the mouse re-enters the window
        // after leaving it. This breaks some of the Aero snap gestures, so we'll capture it to
        // allow us to receive the mouse button up events later.
        //
        // On macOS and X11, capturing the mouse allows us to receive mouse motion outside the
        // window (button up already worked without capture).
        if (m_AbsoluteMouseMode && isCaptureActive()) {
            // NB: Not using SDL_GetGlobalMouseState() because we want our state not the system's
            Uint32 mouseState = SDL_GetMouseState(nullptr, nullptr);
            for (Uint32 button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; button++) {
                if (mouseState & SDL_BUTTON(button)) {
                    SDL_CaptureMouse(SDL_TRUE);
                    break;
                }
            }
        }
    }
}

void SdlInputHandler::notifyFocusLost()
{
    // Release mouse cursor when another window is activated (e.g. by using ALT+TAB).
    // This lets user to interact with our window's title bar and with the buttons in it.
    // Doing this while the window is full-screen breaks the transition out of FS
    // (desktop and exclusive), so we must check for that before releasing mouse capture.
    if (!(SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) && !m_AbsoluteMouseMode) {
        setCaptureActive(false);
    }

    // Raise all keys that are currently pressed. If we don't do this, certain keys
    // used in shortcuts that cause focus loss (such as Alt+Tab) may get stuck down.
    raiseAllKeys();
}

void SdlInputHandler::notifyFocusGained()
{
}

bool SdlInputHandler::isCaptureActive()
{
    if (SDL_GetRelativeMouseMode()) {
        return true;
    }

    // Some platforms don't support SDL_SetRelativeMouseMode
    return m_FakeMouseCaptureActive;
}

void SdlInputHandler::updateKeyboardGrabState()
{
    bool shouldGrab = m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF && isCaptureActive();
    if (shouldGrab) {
        Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
        if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(windowFlags & SDL_WINDOW_FULLSCREEN)) {
            // Ungrab if it's fullscreen only and we left fullscreen
            shouldGrab = false;
        }
    }

    // Don't close the window on Alt+F4 when keyboard grab is enabled
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, shouldGrab ? "1" : "0");

#if SDL_VERSION_ATLEAST(2, 0, 15)
    // On SDL 2.0.15+, we can get keyboard-only grab on Win32, X11, and Wayland.
    // SDL 2.0.18 adds keyboard grab on macOS (if built with non-AppStore APIs).
    SDL_SetWindowKeyboardGrab(m_Window, shouldGrab ? SDL_TRUE : SDL_FALSE);
#endif

    m_KeyboardCaptureActive = shouldGrab;
}

bool SdlInputHandler::isSystemKeyCaptureActive()
{
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF) {
        return false;
    }

    if (m_Window == nullptr) {
        return false;
    }

    // NB: We used to check SDL_WINDOW_KEYBOARD_GRABBED here, but this isn't
    // always set when capture "fails" on SDL3, even though the user may have
    // configured the compositor to pass through system keys to us anyway.
    // See issues #1776 and #1900 for details.
    Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
    if (!(windowFlags & SDL_WINDOW_INPUT_FOCUS) || !m_KeyboardCaptureActive) {
        return false;
    }

    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(windowFlags & SDL_WINDOW_FULLSCREEN)) {
        return false;
    }

    return true;
}

bool SdlInputHandler::keyboardCaptureEnabled() const
{
    return m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF;
}

bool SdlInputHandler::setKeyboardCaptureEnabled(bool enabled)
{
    m_CaptureSystemKeysMode = enabled
        ? StreamingPreferences::CSK_ALWAYS
        : StreamingPreferences::CSK_OFF;
    if (!m_LocalOverlayInputActive.load(std::memory_order_acquire) &&
            m_Window != nullptr) {
        updateKeyboardGrabState();
    }
    return keyboardCaptureEnabled();
}

bool SdlInputHandler::toggleKeyboardCaptureFromShortcut()
{
    return setKeyboardCaptureEnabled(!isSystemKeyCaptureActive());
}

void SdlInputHandler::setCaptureActive(bool active)
{
    if (m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        m_DeferredCaptureActive = active;
        return;
    }
    applyCaptureActive(active);
}

void SdlInputHandler::applyCaptureActive(bool active)
{
    if (active) {
        // If we're in relative mode, try to activate SDL's relative mouse mode
        if (m_AbsoluteMouseMode || SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
            // Relative mouse mode didn't work or was disabled, so we'll just hide the cursor
            SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
            m_FakeMouseCaptureActive = true;
        }

        // Synchronize the client and host cursor when activating absolute capture
        if (m_AbsoluteMouseMode) {
            int mouseX, mouseY;
            int windowX, windowY;

            // We have to use SDL_GetGlobalMouseState() because macOS may not reflect
            // the new position of the mouse when outside the window.
            SDL_GetGlobalMouseState(&mouseX, &mouseY);

            // Convert global mouse state to window-relative
            SDL_GetWindowPosition(m_Window, &windowX, &windowY);
            mouseX -= windowX;
            mouseY -= windowY;

            if (isMouseInVideoRegion(mouseX, mouseY)) {
                // Synthesize a mouse event to synchronize the cursor
                SDL_MouseMotionEvent motionEvent = {};
                motionEvent.type = SDL_MOUSEMOTION;
                motionEvent.timestamp = SDL_GetTicks();
                motionEvent.windowID = SDL_GetWindowID(m_Window);
                motionEvent.x = mouseX;
                motionEvent.y = mouseY;
                handleMouseMotionEvent(&motionEvent);
            }
        }
    }
    else {
        if (m_FakeMouseCaptureActive) {
            // Display the cursor again
            SDL_ShowCursor(SDL_ENABLE);
            m_FakeMouseCaptureActive = false;
        }
        else {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    }

    // Update mouse pointer region constraints
    updatePointerRegionLock();

    // Now update the keyboard grab
    updateKeyboardGrabState();
}

void SdlInputHandler::handleTouchFingerEvent(SDL_TouchFingerEvent* event)
{
    std::lock_guard<std::recursive_mutex> lock(m_RemoteInputMutex);
    if (m_LocalOverlayInputActive.load(std::memory_order_acquire)) {
        return;
    }
#if SDL_VERSION_ATLEAST(2, 0, 10)
    if (SDL_GetTouchDeviceType(event->touchId) != SDL_TOUCH_DEVICE_DIRECT) {
        // Ignore anything that isn't a touchscreen. We may get callbacks
        // for trackpads, but we want to handle those in the mouse path.
        return;
    }
#elif defined(Q_OS_DARWIN)
    // SDL2 sends touch events from trackpads by default on
    // macOS. This totally screws our actual mouse handling,
    // so we must explicitly ignore touch events on macOS
    // until SDL 2.0.10 where we have SDL_GetTouchDeviceType()
    // to tell them apart.
    return;
#endif

    if (m_AbsoluteTouchMode) {
        handleAbsoluteFingerEvent(event);
    }
    else {
        handleRelativeFingerEvent(event);
    }
}
