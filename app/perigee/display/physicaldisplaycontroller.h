#pragma once

#include "perigee/actions/hostadapter.h"

#include <QtGlobal>

#include <atomic>
#include <functional>

class PhysicalDisplayController
{
public:
    static constexpr int MinimumDisplay = 1;
    static constexpr int MaximumDisplay = 13;
    static constexpr qint64 VerificationTimeoutMs = 5000;

    enum class Phase { Idle, WaitingForFrame, Restoring, Succeeded, Failed };
    using Clock = std::function<qint64()>;
    using ShortcutSender = std::function<bool(int)>;
    using Enqueue = std::function<bool(quint64)>;
    using Completion = HostAdapter::Completion;

    PhysicalDisplayController(Clock clock, ShortcutSender sender);

    bool request(int displayNumber, Completion completion);
    bool notifyAcceptedFrame(const Enqueue& enqueue);
    bool observeFreshFrame(quint64 evidenceEpoch);
    bool checkDeadline();
    void cancel();

    int lastRequestedDisplay() const;
    int pendingDisplay() const;
    quint64 evidenceEpoch() const;
    bool active() const;
    Phase phase() const;

private:
    static constexpr quint64 QueuedBit = quint64(1) << 63;
    static constexpr quint64 MaximumEvidenceEpoch = QueuedBit - 1;

    Clock m_Clock;
    ShortcutSender m_Sender;
    Completion m_Completion;
    std::atomic<quint64> m_FrameGate {0};
    quint64 m_NextEvidenceEpoch = 0;
    quint64 m_EvidenceEpoch = 0;
    qint64 m_Deadline = 0;
    int m_RequestedDisplay = 0;
    int m_PreviousDisplay = 0;
    int m_LastRequestedDisplay = 0;
    bool m_RestorationAttempted = false;
    Phase m_Phase = Phase::Idle;

    bool sendAndArm(int displayNumber, Phase phase);
    void disarmFrameGate();
    void finish(ActionResult result);
};
