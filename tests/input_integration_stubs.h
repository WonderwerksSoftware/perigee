#pragma once

#include <QStringList>
#include <QVector>

#include <cstdint>

namespace InputIntegrationStubs {

struct MouseButtonRecord
{
    int action;
    int button;
};

struct TouchRecord { int eventType; quint32 pointerId; };
struct PenRecord { int eventType; };
struct ControllerRecord { int controllerIndex; int buttons; int leftStickX; };
struct BatteryRecord { int controllerIndex; int state; int percentage; };

void reset();
QVector<MouseButtonRecord> mouseButtons();
QVector<TouchRecord> touches();
QVector<PenRecord> pens();
QVector<ControllerRecord> controllers();
QVector<BatteryRecord> batteries();
int mouseMoveCount();
void beginOrderingObservation();
QStringList ordering();
void blockNextMouseButtonSend();
void blockNextMouseMoveSend();
bool waitUntilSendBlocked(int timeoutMs = 1000);
void releaseBlockedSend();

}
