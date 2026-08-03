#include "remoteinputstate.h"

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
    m_MouseButtonsDown.clear();
    return neutral;
}
