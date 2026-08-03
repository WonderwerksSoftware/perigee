#pragma once

#include <QSet>
#include <QVector>

struct NeutralRemoteInput
{
    QVector<short> keyReleases;
    QVector<int> mouseButtonReleases;
    QVector<short> controllerIndicesToZero;
};

class RemoteInputState final
{
public:
    void keySent(short keyCode, bool down);
    void mouseButtonSent(int button, bool down);
    void controllerAllocated(short controllerIndex);
    void controllerRemoved(short controllerIndex);

    bool hasKeysDown() const;
    bool hasMouseButtonsDown() const;
    QVector<short> takeKeyReleases();
    NeutralRemoteInput takeNeutralInput();

private:
    QSet<short> m_KeysDown;
    QSet<int> m_MouseButtonsDown;
    QSet<short> m_AllocatedControllers;
};
