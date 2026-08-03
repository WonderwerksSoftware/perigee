#include "input_integration_stubs.h"

#include "streaming/session.h"
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
bool g_RecordOrdering = false;
bool g_BlockMouseButton = false;
bool g_BlockMouseMove = false;
bool g_SendBlocked = false;
bool g_ReleaseBlockedSend = false;
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
    g_RecordOrdering = false;
    g_BlockMouseButton = false;
    g_BlockMouseMove = false;
    g_SendBlocked = false;
    g_ReleaseBlockedSend = false;
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

bool waitUntilSendBlocked(int timeoutMs)
{
    QMutexLocker locker(&g_Mutex);
    if (!g_SendBlocked) {
        g_SendCondition.wait(&g_Mutex, timeoutMs);
    }
    return g_SendBlocked;
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
int LiSendMouseMoveEvent(short, short)
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
void Session::notifyMouseEmulationMode(bool) {}
void Session::toggleFullscreen() {}
void Session::setShouldExit(bool) {}
