#include "remoteinputstate.h"

#include <Limelight.h>

#include <algorithm>

namespace {

template<typename T>
QVector<T> sortedValues(const QSet<T>& values)
{
    QVector<T> result(values.cbegin(), values.cend());
    std::sort(result.begin(), result.end());
    return result;
}

}

void RemoteInputState::keySent(short keyCode, bool down)
{
    if (down) {
        m_KeysDown.insert(keyCode);
    }
    else {
        m_KeysDown.remove(keyCode);
    }
}

void RemoteInputState::mouseButtonSent(int button, bool down)
{
    if (down) {
        m_MouseButtonsDown.insert(button);
    }
    else {
        m_MouseButtonsDown.remove(button);
    }
}

void RemoteInputState::controllerAllocated(short controllerIndex)
{
    m_AllocatedControllers.insert(controllerIndex);
}

void RemoteInputState::controllerRemoved(short controllerIndex)
{
    m_AllocatedControllers.remove(controllerIndex);
}

void RemoteInputState::touchSent(uint8_t eventType, uint32_t pointerId)
{
    if (eventType == LI_TOUCH_EVENT_DOWN || eventType == LI_TOUCH_EVENT_MOVE) {
        m_ActiveTouches.insert(pointerId);
    }
    else if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
        m_ActiveTouches.remove(pointerId);
    }
    else if (eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
        m_ActiveTouches.clear();
    }
}

void RemoteInputState::penSent(uint8_t eventType)
{
    if (eventType == LI_TOUCH_EVENT_DOWN || eventType == LI_TOUCH_EVENT_MOVE) {
        m_PenActive = true;
    }
    else if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL ||
             eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
        m_PenActive = false;
    }
}

bool RemoteInputState::hasKeysDown() const
{
    return !m_KeysDown.isEmpty();
}

bool RemoteInputState::hasMouseButtonsDown() const
{
    return !m_MouseButtonsDown.isEmpty();
}

QVector<short> RemoteInputState::takeKeyReleases()
{
    QVector<short> releases = sortedValues(m_KeysDown);
    m_KeysDown.clear();
    return releases;
}

NeutralRemoteInput RemoteInputState::takeNeutralInput()
{
    NeutralRemoteInput neutral;
    neutral.keyReleases = takeKeyReleases();
    neutral.mouseButtonReleases = sortedValues(m_MouseButtonsDown);
    neutral.controllerIndicesToZero = sortedValues(m_AllocatedControllers);
    neutral.cancelAllTouches = !m_ActiveTouches.isEmpty();
    neutral.cancelPen = m_PenActive;
    m_MouseButtonsDown.clear();
    m_ActiveTouches.clear();
    m_PenActive = false;
    return neutral;
}
