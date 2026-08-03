#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"

#include "SDL_compat.h"
#include "remoteinputstate.h"

#include <atomic>
#include <QHash>
#include <mutex>
#include <optional>

struct CaptureSnapshot {
    bool mouseCaptured;
    bool keyboardCaptured;
};

class SdlInputHandler;

struct GamepadState {
    SDL_GameController* controller;
    SDL_JoystickID jsId;
    short index;

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_Haptic* haptic;
    int hapticMethod;
    int hapticEffectId;
#endif

    SDL_TimerID mouseEmulationTimer;
    uint32_t mouseEmulationTimerToken;
    uint32_t lastStartDownTime;

    bool clickpadButtonEmulationEnabled;
    bool emulatedClickpadButtonDown;

#if SDL_VERSION_ATLEAST(2, 0, 14)
    uint8_t gyroReportPeriodMs;
    float lastGyroEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastGyroEventTime;

    uint8_t accelReportPeriodMs;
    float lastAccelEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastAccelEventTime;
#endif

    int buttons;
    short lsX, lsY;
    short rsX, rsY;
    unsigned char lt, rt;
};


struct DualSenseOutputReport{
    uint8_t validFlag0;
    uint8_t validFlag1;

    /* For DualShock 4 compatibility mode. */
    uint8_t motorRight;
    uint8_t motorLeft;

    /* Audio controls */
    uint8_t reserved[4];
    uint8_t muteButtonLed;

    uint8_t powerSaveControl;
    uint8_t rightTriggerEffectType;
    uint8_t rightTriggerEffect[DS_EFFECT_PAYLOAD_SIZE];
    uint8_t leftTriggerEffectType;
    uint8_t leftTriggerEffect[DS_EFFECT_PAYLOAD_SIZE];
    uint8_t reserved2[6];

    /* LEDs and lightbar */
    uint8_t validFlag2;
    uint8_t reserved3[2];
    uint8_t lightbarSetup;
    uint8_t ledBrightness;
    uint8_t playerLeds;
    uint8_t lightbarRed;
    uint8_t lightbarGreen;
    uint8_t lightbarBlue;
};

// activeGamepadMask is a short, so we're bounded by the number of mask bits
#define MAX_GAMEPADS 16

#define MAX_FINGERS 2

#define GAMEPAD_HAPTIC_METHOD_NONE 0
#define GAMEPAD_HAPTIC_METHOD_LEFTRIGHT 1
#define GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE 2

#define GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT 0.33
#define GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT 0.8

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs, int streamWidth, int streamHeight);

    ~SdlInputHandler();

    void setWindow(SDL_Window* window);

    void handleKeyEvent(SDL_KeyboardEvent* event);

    void handleMouseButtonEvent(SDL_MouseButtonEvent* event);

    void handleMouseMotionEvent(SDL_MouseMotionEvent* event);

    void handleMouseWheelEvent(SDL_MouseWheelEvent* event);

    void handleControllerAxisEvent(SDL_ControllerAxisEvent* event);

    void handleControllerButtonEvent(SDL_ControllerButtonEvent* event);

    void handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event);

#if SDL_VERSION_ATLEAST(2, 0, 14)
    void handleControllerSensorEvent(SDL_ControllerSensorEvent* event);

    void handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event);
#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)
    void handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event);
#endif

    void handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event);

    void sendText(QString& string);

    void rumble(uint16_t controllerNumber, uint16_t lowFreqMotor, uint16_t highFreqMotor);

    void rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    void setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    void setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    void setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report);

    void handleTouchFingerEvent(SDL_TouchFingerEvent* event);

    bool handleInputTimerEvent(const SDL_UserEvent& event);

    int getAttachedGamepadMask();

    void raiseAllKeys();

    CaptureSnapshot beginLocalOverlayInput();

    void endLocalOverlayInput(CaptureSnapshot snapshot, bool keepReleased);

    void sendNeutralRemoteInput();

    bool sendNeutralControllerInput(SDL_JoystickID id);

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    bool isSystemKeyCaptureActive();

    void setCaptureActive(bool active);

    bool isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth = -1, int windowHeight = -1);

    void updateKeyboardGrabState();

    void updatePointerRegionLock();

    static
    QString getUnmappedGamepads();

private:
    static constexpr Uint32 MouseEmulationPollingInterval = 50;
    static constexpr float MouseEmulationMotionMultiplier = 4.0f;
    static constexpr float MouseEmulationDeadzone = 2.0f;

    enum class InputTimerAction {
        LongPress,
        ReleaseLeftButton,
        ReleaseRightButton,
        Drag,
        MouseEmulation,
    };

    struct InputTimerRequest {
        InputTimerAction action;
        SDL_JoystickID controllerId;
        bool repeating;
    };

    enum KeyCombo {
        KeyComboQuit,
        KeyComboUngrabInput,
        KeyComboToggleFullScreen,
        KeyComboToggleStatsOverlay,
        KeyComboToggleMouseMode,
        KeyComboToggleCursorHide,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboQuitAndExit,
        KeyComboToggleKeyboardGrab,
        KeyComboMax
    };

    GamepadState*
    findStateForGamepad(SDL_JoystickID id);

    void sendGamepadState(GamepadState* state);

    void sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level);

    void handleAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void emulateAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void disableTouchFeedback();

    void handleRelativeFingerEvent(SDL_TouchFingerEvent* event);

    void performSpecialKeyCombo(KeyCombo combo);

    void sendTrackedMouseButtonEvent(int action, int button);

    void sendTrackedKeyboardEvent(short keyCode, char action,
                                  char modifiers, char flags);

    void sendTrackedTouchEvent(uint8_t eventType, uint32_t pointerId,
                               float x, float y, float pressure);

    void sendTrackedPenEvent(uint8_t eventType, float x, float y,
                             float pressure);

    void applyCaptureActive(bool active);

    bool startInputTimer(SDL_TimerID& timer, uint32_t& token,
                         Uint32 interval, InputTimerAction action,
                         SDL_JoystickID controllerId = 0,
                         bool repeating = false);

    void cancelInputTimer(SDL_TimerID& timer, uint32_t& token);

    void dispatchInputTimer(const InputTimerRequest& request);

    static
    bool pushInputTimerEvent(void* param);

    static
    Uint32 longPressTimerCallback(Uint32 interval, void* param);

    static
    Uint32 mouseEmulationTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseLeftButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseRightButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 dragTimerCallback(Uint32 interval, void* param);

    SDL_Window* m_Window;
    bool m_MultiController;
    bool m_GamepadMouse;
    bool m_SwapMouseButtons;
    bool m_ReverseScrollDirection;
    bool m_SwapFaceButtons;

    bool m_NeedsManualCaptureOnLeave;
    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;

    int m_GamepadMask;
    GamepadState m_GamepadState[MAX_GAMEPADS];
    RemoteInputState m_RemoteInputState;
    // Controller handlers can re-enter tracked-send helpers and targeted
    // neutralization while preserving one ownership transaction.
    std::recursive_mutex m_RemoteInputMutex;
    std::atomic_bool m_LocalOverlayInputActive {false};
    std::optional<bool> m_DeferredCaptureActive;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    QString m_OldIgnoreDevices;
    QString m_OldIgnoreDevicesExcept;
    QStringList m_IgnoreDeviceGuids;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    int m_MouseCursorCapturedVisibilityState;

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    SDL_TouchFingerEvent m_LastTouchDownEvent;
    SDL_TouchFingerEvent m_LastTouchUpEvent;
    SDL_TimerID m_LongPressTimer;
    uint32_t m_LongPressTimerToken;
    int m_StreamWidth;
    int m_StreamHeight;
    bool m_AbsoluteMouseMode;
    bool m_AbsoluteTouchMode;
    bool m_DisabledTouchFeedback;

    SDL_TouchFingerEvent m_TouchDownEvent[MAX_FINGERS];
    SDL_TimerID m_LeftButtonReleaseTimer;
    uint32_t m_LeftButtonReleaseTimerToken;
    SDL_TimerID m_RightButtonReleaseTimer;
    uint32_t m_RightButtonReleaseTimerToken;
    SDL_TimerID m_DragTimer;
    uint32_t m_DragTimerToken;
    char m_DragButton;
    int m_NumFingersDown;

    QHash<uint32_t, InputTimerRequest> m_InputTimerRequests;

    static const int k_ButtonMap[];
};
