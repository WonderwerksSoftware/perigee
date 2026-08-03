#include "input_integration_stubs.h"

namespace WMUtils {

bool isRunningWayland()
{
    return false;
}

bool isGpuSlow()
{
    return false;
}

}

#include "streaming/session.h"
#include "streaming/sdleventcodes.h"
#include "streaming/streamutils.h"
#include "settings/mappingmanager.h"
#include "utils.h"

#include <Limelight.h>

#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

namespace {

QMutex g_Mutex;
QVector<InputIntegrationStubs::MouseButtonRecord> g_MouseButtons;
QVector<InputIntegrationStubs::TouchRecord> g_Touches;
QVector<InputIntegrationStubs::PenRecord> g_Pens;
QVector<InputIntegrationStubs::ControllerRecord> g_Controllers;
QVector<InputIntegrationStubs::BatteryRecord> g_Batteries;
QStringList g_Ordering;
int g_MouseMoveCount = 0;
QVector<InputIntegrationStubs::MouseMoveRecord> g_MouseMoves;
QVector<bool> g_MouseEmulationNotifications;
bool g_RecordOrdering = false;
bool g_BlockMouseButton = false;
bool g_BlockMouseMove = false;
bool g_BlockInputTimerPush = false;
bool g_SendBlocked = false;
bool g_InputTimerPushBlocked = false;
bool g_ReleaseBlockedSend = false;
bool g_FailNextTimerAdd = false;
bool g_FailNextInputTimerPush = false;
QWaitCondition g_SendCondition;

}

extern "C" int __wrap_SDL_GetNumTouchFingers(SDL_TouchID)
{
    return 0;
}

extern "C" int __real_SDL_SetRelativeMouseMode(SDL_bool enabled);
extern "C" int __wrap_SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    {
        QMutexLocker locker(&g_Mutex);
        if (g_RecordOrdering) {
            g_Ordering.append(enabled ? QStringLiteral("capture-on")
                                      : QStringLiteral("capture-off"));
        }
    }
    return __real_SDL_SetRelativeMouseMode(enabled);
}

extern "C" int __real_SDL_PushEvent(SDL_Event* event);
extern "C" int __wrap_SDL_PushEvent(SDL_Event* event)
{
    {
        QMutexLocker locker(&g_Mutex);
        if (g_FailNextInputTimerPush && event != nullptr &&
                event->type == SDL_USEREVENT &&
                event->user.code == SDL_CODE_INPUT_TIMER) {
            g_FailNextInputTimerPush = false;
            return -1;
        }
        if (g_BlockInputTimerPush && event != nullptr &&
                event->type == SDL_USEREVENT &&
                event->user.code == SDL_CODE_INPUT_TIMER) {
            g_BlockInputTimerPush = false;
            g_InputTimerPushBlocked = true;
            g_SendCondition.wakeAll();
            while (!g_ReleaseBlockedSend) {
                g_SendCondition.wait(&g_Mutex);
            }
            g_InputTimerPushBlocked = false;
            g_SendCondition.wakeAll();
        }
    }
    return __real_SDL_PushEvent(event);
}

extern "C" SDL_TimerID __real_SDL_AddTimer(Uint32 interval,
                                             SDL_TimerCallback callback,
                                             void* param);
extern "C" SDL_TimerID __wrap_SDL_AddTimer(Uint32 interval,
                                             SDL_TimerCallback callback,
                                             void* param)
{
    {
        QMutexLocker locker(&g_Mutex);
        if (g_FailNextTimerAdd) {
            g_FailNextTimerAdd = false;
            return 0;
        }
    }
    return __real_SDL_AddTimer(interval, callback, param);
}

namespace InputIntegrationStubs {

void reset()
{
    QMutexLocker locker(&g_Mutex);
    g_MouseButtons.clear();
    g_Touches.clear();
    g_Pens.clear();
    g_Controllers.clear();
    g_Batteries.clear();
    g_Ordering.clear();
    g_MouseMoveCount = 0;
    g_MouseMoves.clear();
    g_MouseEmulationNotifications.clear();
    g_RecordOrdering = false;
    g_BlockMouseButton = false;
    g_BlockMouseMove = false;
    g_BlockInputTimerPush = false;
    g_SendBlocked = false;
    g_InputTimerPushBlocked = false;
    g_ReleaseBlockedSend = false;
    g_FailNextTimerAdd = false;
    g_FailNextInputTimerPush = false;
}

QVector<MouseButtonRecord> mouseButtons()
{
    QMutexLocker locker(&g_Mutex);
    return g_MouseButtons;
}

QVector<TouchRecord> touches()
{
    QMutexLocker locker(&g_Mutex);
    return g_Touches;
}

QVector<PenRecord> pens()
{
    QMutexLocker locker(&g_Mutex);
    return g_Pens;
}

QVector<ControllerRecord> controllers()
{
    QMutexLocker locker(&g_Mutex);
    return g_Controllers;
}

QVector<BatteryRecord> batteries()
{
    QMutexLocker locker(&g_Mutex);
    return g_Batteries;
}

int mouseMoveCount()
{
    QMutexLocker locker(&g_Mutex);
    return g_MouseMoveCount;
}

QVector<MouseMoveRecord> mouseMoves()
{
    QMutexLocker locker(&g_Mutex);
    return g_MouseMoves;
}

QVector<bool> mouseEmulationNotifications()
{
    QMutexLocker locker(&g_Mutex);
    return g_MouseEmulationNotifications;
}

void beginOrderingObservation()
{
    QMutexLocker locker(&g_Mutex);
    g_Ordering.clear();
    g_RecordOrdering = true;
}

QStringList ordering()
{
    QMutexLocker locker(&g_Mutex);
    return g_Ordering;
}

void blockNextMouseButtonSend()
{
    QMutexLocker locker(&g_Mutex);
    g_BlockMouseButton = true;
    g_ReleaseBlockedSend = false;
}

void blockNextMouseMoveSend()
{
    QMutexLocker locker(&g_Mutex);
    g_BlockMouseMove = true;
    g_ReleaseBlockedSend = false;
}

void blockNextTimerCallbackSideEffect()
{
    QMutexLocker locker(&g_Mutex);
    g_BlockMouseButton = true;
    g_BlockMouseMove = true;
    g_BlockInputTimerPush = true;
    g_ReleaseBlockedSend = false;
}

void failNextTimerAdd()
{
    QMutexLocker locker(&g_Mutex);
    g_FailNextTimerAdd = true;
}

void failNextInputTimerPush()
{
    QMutexLocker locker(&g_Mutex);
    g_FailNextInputTimerPush = true;
}

bool waitUntilSendBlocked(int timeoutMs)
{
    QMutexLocker locker(&g_Mutex);
    if (!g_SendBlocked) {
        g_SendCondition.wait(&g_Mutex, timeoutMs);
    }
    return g_SendBlocked;
}

TimerCallbackBlock waitUntilTimerCallbackBlocked(int timeoutMs)
{
    QMutexLocker locker(&g_Mutex);
    if (!g_SendBlocked && !g_InputTimerPushBlocked) {
        g_SendCondition.wait(&g_Mutex, timeoutMs);
    }
    if (g_InputTimerPushBlocked) {
        return TimerCallbackBlock::InputEventPush;
    }
    if (g_SendBlocked) {
        return TimerCallbackBlock::RemoteSend;
    }
    return TimerCallbackBlock::None;
}

bool waitUntilTimerCallbackReleased(int timeoutMs)
{
    QMutexLocker locker(&g_Mutex);
    if (g_InputTimerPushBlocked) {
        g_SendCondition.wait(&g_Mutex, timeoutMs);
    }
    return !g_InputTimerPushBlocked;
}

void releaseBlockedSend()
{
    QMutexLocker locker(&g_Mutex);
    g_ReleaseBlockedSend = true;
    g_SendCondition.wakeAll();
}

}

extern "C" {

int LiSendKeyboardEvent(short, char, char) { return 0; }
int LiSendKeyboardEvent2(short, char, char, char) { return 0; }
int LiSendUtf8TextEvent(const char*, unsigned int) { return 0; }
int LiSendMouseButtonEvent(char action, int button)
{
    QMutexLocker locker(&g_Mutex);
    if (g_BlockMouseButton) {
        g_BlockMouseButton = false;
        g_SendBlocked = true;
        g_SendCondition.wakeAll();
        while (!g_ReleaseBlockedSend) {
            g_SendCondition.wait(&g_Mutex);
        }
        g_SendBlocked = false;
    }
    g_MouseButtons.push_back({action, button});
    if (g_RecordOrdering) {
        g_Ordering.append(action == BUTTON_ACTION_RELEASE
                              ? QStringLiteral("remote-mouse-release")
                              : QStringLiteral("remote-mouse-press"));
    }
    return 0;
}
int LiSendMouseMoveEvent(short x, short y)
{
    QMutexLocker locker(&g_Mutex);
    if (g_BlockMouseMove) {
        g_BlockMouseMove = false;
        g_SendBlocked = true;
        g_SendCondition.wakeAll();
        while (!g_ReleaseBlockedSend) {
            g_SendCondition.wait(&g_Mutex);
        }
        g_SendBlocked = false;
    }
    ++g_MouseMoveCount;
    g_MouseMoves.append({x, y});
    return 0;
}
int LiSendMousePositionEvent(short, short, short, short) { return 0; }
int LiSendMultiControllerEvent(short controllerIndex, short, int buttons,
                               unsigned char, unsigned char, short leftStickX,
                               short, short, short)
{
    QMutexLocker locker(&g_Mutex);
    g_Controllers.push_back({controllerIndex, buttons, leftStickX});
    return 0;
}
int LiSendScrollEvent(signed char) { return 0; }
int LiSendHScrollEvent(signed char) { return 0; }
int LiSendHighResScrollEvent(short) { return 0; }
int LiSendHighResHScrollEvent(short) { return 0; }
int LiSendControllerArrivalEvent(uint8_t, uint16_t, uint8_t, uint32_t, uint16_t)
{ return 0; }
int LiSendControllerBatteryEvent(uint8_t controllerIndex, uint8_t state,
                                 uint8_t percentage)
{
    QMutexLocker locker(&g_Mutex);
    g_Batteries.push_back({controllerIndex, state, percentage});
    return 0;
}
int LiSendControllerMotionEvent(uint8_t, uint8_t, float, float, float) { return 0; }
int LiSendControllerTouchEvent2(uint8_t, uint8_t, uint8_t, uint32_t,
                                float, float, float) { return 0; }
int LiSendTouchEvent(uint8_t eventType, uint32_t pointerId, float, float, float,
                     float, float, uint16_t)
{
    QMutexLocker locker(&g_Mutex);
    g_Touches.push_back({eventType, pointerId});
    return 0;
}
int LiSendPenEvent(uint8_t eventType, uint8_t, uint8_t, float, float, float,
                   float, float, uint16_t, uint8_t)
{
    QMutexLocker locker(&g_Mutex);
    g_Pens.push_back({eventType});
    return 0;
}
uint32_t LiGetHostFeatureFlags(void) { return LI_FF_PEN_TOUCH_EVENTS; }

}

MappingFetcher* MappingManager::s_MappingFetcher = nullptr;
MappingManager::MappingManager() = default;
void MappingManager::applyMappings() {}

namespace WMUtils {
bool isRunningDesktopEnvironment() { return true; }
}

void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst)
{
    const float sourceAspect = float(src->w) / src->h;
    const float destinationAspect = float(dst->w) / dst->h;
    if (destinationAspect > sourceAspect) {
        const int width = int(dst->h * sourceAspect);
        dst->x += (dst->w - width) / 2;
        dst->w = width;
    }
    else {
        const int height = int(dst->w / sourceAspect);
        dst->y += (dst->h - height) / 2;
        dst->h = height;
    }
}

Session* Session::s_ActiveSession = nullptr;
void Session::notifyMouseEmulationMode(bool enabled)
{
    QMutexLocker locker(&g_Mutex);
    g_MouseEmulationNotifications.append(enabled);
}
void Session::toggleStatsOverlay() {}
void Session::toggleFullscreen() {}
void Session::setShouldExit(bool) {}
