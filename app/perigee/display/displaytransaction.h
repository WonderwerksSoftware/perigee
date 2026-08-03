#pragma once

#include "perigee/polaris/polarismodels.h"

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <functional>

enum class DisplayPhase {
    Idle,
    Selecting,
    Disconnecting,
    Reconnecting,
    Verifying,
    RollingBack,
    Succeeded,
    Failed,
};

enum class DisplayOutcome {
    None,
    RequestedTargetActive,
    PreviousTargetRestored,
    RollbackFailed,
    SwitchFailed,
    Cancelled,
};

class FirstFrameNotificationGate
{
public:
    static constexpr quint64 MaximumEvidenceEpoch =
        (quint64(1) << 63) - 1;

    void arm(quint64 evidenceEpoch);
    bool notifyAcceptedFrame(
        const std::function<bool(quint64 evidenceEpoch)>& enqueue);
    quint64 armedEvidenceEpoch() const;
    quint64 queuedEvidenceEpoch() const;
    void disarm();

private:
    static constexpr quint64 QueuedBit = quint64(1) << 63;
    std::atomic<quint64> m_State {0};
};

class DisplayTransaction
{
public:
    bool begin(const DisplayTarget& previousTarget,
               const DisplayTarget& requestedTarget,
               const QString& sessionToken,
               quint64 transactionEpoch,
               quint64 sessionEpoch);

    static QByteArray postBody(const DisplayTarget& target,
                               const QString& sessionToken);
    static QVariantMap catalogIdentity(
        const PolarisDiscoverySnapshot& discovery);

    bool onPostAccepted(quint64 transactionEpoch);
    bool onPostRejected(quint64 transactionEpoch, const QString& reason);
    bool onIntentionalDisconnect(quint64 transactionEpoch,
                                 quint64 sessionEpoch);
    bool onSessionAttached(quint64 transactionEpoch, quint64 sessionEpoch);
    bool observeReadback(quint64 transactionEpoch, quint64 sessionEpoch,
                         const DisplayTarget& target);
    bool observeFirstFrame(quint64 transactionEpoch, quint64 sessionEpoch);
    bool beginRollback(quint64 transactionEpoch, const QString& reason);
    bool onRollbackPostAccepted(quint64 transactionEpoch,
                                bool forceReconnect = false);
    bool onRollbackFailed(quint64 transactionEpoch, const QString& reason);
    bool cancelBeforeMutation(quint64 transactionEpoch);
    bool cancelUnexpectedDisconnect(quint64 sessionEpoch);

    DisplayPhase phase() const;
    DisplayOutcome outcome() const;
    QVector<DisplayPhase> phaseHistory() const;
    const DisplayTarget& previousTarget() const;
    const DisplayTarget& requestedTarget() const;
    const DisplayTarget& verificationTarget() const;
    quint64 transactionEpoch() const;
    quint64 sessionEpoch() const;
    bool active() const;
    bool rollbackAttempted() const;
    bool verifyingRollback() const;
    bool requestedTargetSucceeded() const;
    bool hostMayHaveMutated() const;
    bool sessionTokenMatches(const QString& token) const;
    QString failureReason() const;

private:
    bool validTransactionEpoch(quint64 epoch) const;
    bool validEvidenceEpoch(quint64 transactionEpoch,
                            quint64 sessionEpoch) const;
    bool completeIfVerified();
    void setPhase(DisplayPhase phase);

    DisplayTarget m_PreviousTarget;
    DisplayTarget m_RequestedTarget;
    QString m_SessionToken;
    DisplayPhase m_Phase = DisplayPhase::Idle;
    DisplayOutcome m_Outcome = DisplayOutcome::None;
    QVector<DisplayPhase> m_PhaseHistory {DisplayPhase::Idle};
    quint64 m_TransactionEpoch = 0;
    quint64 m_SessionEpoch = 0;
    bool m_FirstFrameDecoded = false;
    bool m_ServerTargetVerified = false;
    bool m_RollbackAttempted = false;
    bool m_VerifyingRollback = false;
    bool m_HostMayHaveMutated = false;
    QString m_FailureReason;
};
