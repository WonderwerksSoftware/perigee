#pragma once

#include <QSet>
#include <QVector>

struct NeutralRemoteInput
{
    QVector<short> keyReleases;
    QVector<int> mouseButtonReleases;
    QVector<short> controllerIndicesToZero;
    bool cancelAllTouches = false;
    bool cancelPen = false;
};

class RemoteInputState final
{
public:
    void keySent(short keyCode, bool down);
    void mouseButtonSent(int button, bool down);
    void controllerAllocated(short controllerIndex);
    void controllerRemoved(short controllerIndex);
    void touchSent(uint8_t eventType, uint32_t pointerId);
    void penSent(uint8_t eventType);

    bool hasKeysDown() const;
    bool hasMouseButtonsDown() const;
    QVector<short> takeKeyReleases();
    NeutralRemoteInput takeNeutralInput();

private:
    QSet<short> m_KeysDown;
    QSet<int> m_MouseButtonsDown;
    QSet<short> m_AllocatedControllers;
    QSet<uint32_t> m_ActiveTouches;
    bool m_PenActive = false;
};
