#pragma once

#include <QStringList>
#include <QVector>
#include <QDebug>

#include <cstdint>

namespace InputIntegrationStubs {

enum class TimerCallbackBlock {
    None,
    RemoteSend,
    InputEventPush,
};

struct MouseButtonRecord
{
    int action;
    int button;
};

struct KeyboardRecord
{
    int keyCode;
    int action;
    int modifiers;
    int flags;

    bool operator==(const KeyboardRecord& other) const
    {
        return keyCode == other.keyCode && action == other.action
            && modifiers == other.modifiers && flags == other.flags;
    }
};

inline QDebug operator<<(QDebug debug, const KeyboardRecord& record)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "KeyboardRecord(" << Qt::hex << record.keyCode
                    << Qt::dec << ", " << record.action << ", "
                    << record.modifiers << ", " << record.flags << ')';
    return debug;
}

struct MouseMoveRecord { short x; short y; };

struct TouchRecord { int eventType; quint32 pointerId; };
struct PenRecord { int eventType; };
struct ControllerRecord { int controllerIndex; int buttons; int leftStickX; };
struct BatteryRecord { int controllerIndex; int state; int percentage; };

void reset();
QVector<MouseButtonRecord> mouseButtons();
QVector<KeyboardRecord> keyboards();
QVector<TouchRecord> touches();
QVector<PenRecord> pens();
QVector<ControllerRecord> controllers();
QVector<BatteryRecord> batteries();
int mouseMoveCount();
QVector<MouseMoveRecord> mouseMoves();
QVector<bool> mouseEmulationNotifications();
QStringList sessionActions();
void beginOrderingObservation();
QStringList ordering();
void blockNextMouseButtonSend();
void blockNextMouseMoveSend();
void blockNextTimerCallbackSideEffect();
void failNextInputTimerPush();
void failNextTimerAdd();
void failKeyboardSendAt(int zeroBasedPacketIndex);
bool waitUntilSendBlocked(int timeoutMs = 1000);
TimerCallbackBlock waitUntilTimerCallbackBlocked(int timeoutMs = 1000);
bool waitUntilTimerCallbackReleased(int timeoutMs = 1000);
void releaseBlockedSend();

}
