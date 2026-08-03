#include "displaytransaction.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

void FirstFrameNotificationGate::arm(quint64 evidenceEpoch)
{
    if (evidenceEpoch == 0 || evidenceEpoch > MaximumEvidenceEpoch) {
        disarm();
        return;
    }
    m_State.store(evidenceEpoch, std::memory_order_release);
}

bool FirstFrameNotificationGate::notifyAcceptedFrame(
    const std::function<bool(quint64 evidenceEpoch)>& enqueue)
{
    quint64 state = m_State.load(std::memory_order_acquire);
    if (state == 0 || (state & QueuedBit) != 0) {
        return false;
    }

    const quint64 evidenceEpoch = state;
    const quint64 queuedState = evidenceEpoch | QueuedBit;
    if (!m_State.compare_exchange_strong(
            state, queuedState, std::memory_order_acq_rel)) {
        return false;
    }

    if (enqueue && enqueue(evidenceEpoch)) {
        return true;
    }

    // Only release this attempt's one-shot. If arm() published a newer epoch
    // while enqueue was running, this compare/exchange leaves it untouched.
    state = queuedState;
    m_State.compare_exchange_strong(
        state, evidenceEpoch, std::memory_order_acq_rel);
    return false;
}

quint64 FirstFrameNotificationGate::armedEvidenceEpoch() const
{
    return m_State.load(std::memory_order_acquire) & MaximumEvidenceEpoch;
}

quint64 FirstFrameNotificationGate::queuedEvidenceEpoch() const
{
    const quint64 state = m_State.load(std::memory_order_acquire);
    return (state & QueuedBit) != 0 ? state & MaximumEvidenceEpoch : 0;
}

void FirstFrameNotificationGate::disarm()
{
    m_State.store(0, std::memory_order_release);
}

bool DisplayTransaction::begin(const DisplayTarget& previousTarget,
                               const DisplayTarget& requestedTarget,
                               const QString& sessionToken,
                               quint64 transactionEpoch,
                               quint64 sessionEpoch)
{
    if (active() || transactionEpoch == 0 || sessionEpoch == 0 ||
            sessionToken.isEmpty() || !previousTarget.available ||
            !previousTarget.current || !requestedTarget.available ||
            requestedTarget.current || previousTarget.kind != requestedTarget.kind ||
            previousTarget.stableKey() == requestedTarget.stableKey()) {
        return false;
    }
    m_PreviousTarget = previousTarget;
    m_RequestedTarget = requestedTarget;
    m_SessionToken = sessionToken;
    m_TransactionEpoch = transactionEpoch;
    m_SessionEpoch = sessionEpoch;
    m_FirstFrameDecoded = false;
    m_ServerTargetVerified = false;
    m_RollbackAttempted = false;
    m_VerifyingRollback = false;
    m_HostMayHaveMutated = false;
    m_Outcome = DisplayOutcome::None;
    m_FailureReason.clear();
    m_Phase = DisplayPhase::Idle;
    m_PhaseHistory = {DisplayPhase::Idle};
    setPhase(DisplayPhase::Selecting);
    return true;
}

QByteArray DisplayTransaction::postBody(const DisplayTarget& target,
                                        const QString& sessionToken)
{
    if (sessionToken.isEmpty() || target.id.isEmpty()) {
        return {};
    }
    QString field;
    if (target.kind == QStringLiteral("output")) {
        field = QStringLiteral("output_name");
    }
    else if (target.kind == QStringLiteral("stream-mode")) {
        field = QStringLiteral("stream_display_mode");
    }
    else {
        return {};
    }
    QJsonObject object;
    object.insert(field, target.id);
    object.insert(QStringLiteral("session_token"), sessionToken);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QVariantMap DisplayTransaction::catalogIdentity(
    const PolarisDiscoverySnapshot& discovery)
{
    QJsonArray orderedTargets;
    for (const DisplayTarget& target : discovery.settings.targets) {
        QJsonObject serialized;
        serialized.insert(QStringLiteral("kind"), target.kind);
        serialized.insert(QStringLiteral("id"), target.id);
        serialized.insert(QStringLiteral("label"), target.label);
        serialized.insert(QStringLiteral("available"), target.available);
        serialized.insert(QStringLiteral("current"), target.current);
        serialized.insert(QStringLiteral("requires_reconnect"),
                          target.requiresReconnect);
        serialized.insert(QStringLiteral("unavailable_reason"),
                          target.unavailableReason);
        orderedTargets.append(serialized);
    }
    const QByteArray serialized =
        QJsonDocument(orderedTargets).toJson(QJsonDocument::Compact);
    const QByteArray fingerprint = QCryptographicHash::hash(
        serialized, QCryptographicHash::Sha256).toHex();
    return {
        {QStringLiteral("generation"),
         QString::number(discovery.generation)},
        {QStringLiteral("order_sha256"),
         QString::fromLatin1(fingerprint)},
    };
}

bool DisplayTransaction::onPostAccepted(quint64 transactionEpoch)
{
    if (!validTransactionEpoch(transactionEpoch) ||
            m_Phase != DisplayPhase::Selecting) {
        return false;
    }
    m_HostMayHaveMutated = true;
    m_VerifyingRollback = false;
    m_FirstFrameDecoded = false;
    m_ServerTargetVerified = false;
    setPhase(m_RequestedTarget.requiresReconnect
                 ? DisplayPhase::Disconnecting
                 : DisplayPhase::Verifying);
    return true;
}

bool DisplayTransaction::onPostRejected(quint64 transactionEpoch,
                                        const QString& reason)
{
    if (!validTransactionEpoch(transactionEpoch) ||
            m_Phase != DisplayPhase::Selecting || m_HostMayHaveMutated) {
        return false;
    }
    m_FailureReason = reason;
    m_Outcome = DisplayOutcome::SwitchFailed;
    setPhase(DisplayPhase::Failed);
    m_SessionToken.clear();
    return true;
}

bool DisplayTransaction::onIntentionalDisconnect(quint64 transactionEpoch,
                                                  quint64 sessionEpoch)
{
    if (!validEvidenceEpoch(transactionEpoch, sessionEpoch) ||
            m_Phase != DisplayPhase::Disconnecting) {
        return false;
    }
    setPhase(DisplayPhase::Reconnecting);
    return true;
}

bool DisplayTransaction::onSessionAttached(quint64 transactionEpoch,
                                           quint64 sessionEpoch)
{
    if (!validTransactionEpoch(transactionEpoch) || sessionEpoch == 0 ||
            sessionEpoch == m_SessionEpoch ||
            m_Phase != DisplayPhase::Reconnecting) {
        return false;
    }
    m_SessionEpoch = sessionEpoch;
    m_FirstFrameDecoded = false;
    m_ServerTargetVerified = false;
    setPhase(DisplayPhase::Verifying);
    return true;
}

bool DisplayTransaction::observeReadback(quint64 transactionEpoch,
                                         quint64 sessionEpoch,
                                         const DisplayTarget& target)
{
    if (!validEvidenceEpoch(transactionEpoch, sessionEpoch) ||
            m_Phase != DisplayPhase::Verifying ||
            target.stableKey() != verificationTarget().stableKey() ||
            !target.current) {
        return false;
    }
    m_ServerTargetVerified = true;
    completeIfVerified();
    return true;
}

bool DisplayTransaction::observeFirstFrame(quint64 transactionEpoch,
                                           quint64 sessionEpoch)
{
    if (!validEvidenceEpoch(transactionEpoch, sessionEpoch) ||
            m_Phase != DisplayPhase::Verifying) {
        return false;
    }
    m_FirstFrameDecoded = true;
    completeIfVerified();
    return true;
}

bool DisplayTransaction::beginRollback(quint64 transactionEpoch,
                                       const QString& reason)
{
    if (!validTransactionEpoch(transactionEpoch) || !m_HostMayHaveMutated ||
            m_RollbackAttempted || m_Phase == DisplayPhase::Succeeded ||
            m_Phase == DisplayPhase::Failed || m_Phase == DisplayPhase::Idle) {
        return false;
    }
    m_RollbackAttempted = true;
    m_VerifyingRollback = true;
    m_FirstFrameDecoded = false;
    m_ServerTargetVerified = false;
    m_FailureReason = reason;
    setPhase(DisplayPhase::RollingBack);
    return true;
}

bool DisplayTransaction::onRollbackPostAccepted(quint64 transactionEpoch,
                                                 bool forceReconnect)
{
    if (!validTransactionEpoch(transactionEpoch) ||
            m_Phase != DisplayPhase::RollingBack || !m_RollbackAttempted) {
        return false;
    }
    setPhase((m_PreviousTarget.requiresReconnect || forceReconnect)
                 ? DisplayPhase::Disconnecting
                 : DisplayPhase::Verifying);
    return true;
}

bool DisplayTransaction::onRollbackFailed(quint64 transactionEpoch,
                                          const QString& reason)
{
    if (!validTransactionEpoch(transactionEpoch) || !m_RollbackAttempted ||
            (m_Phase != DisplayPhase::RollingBack &&
             m_Phase != DisplayPhase::Disconnecting &&
             m_Phase != DisplayPhase::Reconnecting &&
             m_Phase != DisplayPhase::Verifying)) {
        return false;
    }
    m_FailureReason = reason;
    m_Outcome = DisplayOutcome::RollbackFailed;
    setPhase(DisplayPhase::Failed);
    m_SessionToken.clear();
    return true;
}

bool DisplayTransaction::cancelBeforeMutation(quint64 transactionEpoch)
{
    if (!validTransactionEpoch(transactionEpoch) ||
            m_Phase != DisplayPhase::Selecting || m_HostMayHaveMutated) {
        return false;
    }
    m_Outcome = DisplayOutcome::Cancelled;
    setPhase(DisplayPhase::Idle);
    m_SessionToken.clear();
    return true;
}

bool DisplayTransaction::cancelUnexpectedDisconnect(quint64 sessionEpoch)
{
    if (!active() || sessionEpoch != m_SessionEpoch ||
            m_Phase == DisplayPhase::Disconnecting) {
        return false;
    }
    m_Outcome = DisplayOutcome::Cancelled;
    setPhase(DisplayPhase::Idle);
    m_SessionToken.clear();
    return true;
}

DisplayPhase DisplayTransaction::phase() const { return m_Phase; }
DisplayOutcome DisplayTransaction::outcome() const { return m_Outcome; }
QVector<DisplayPhase> DisplayTransaction::phaseHistory() const { return m_PhaseHistory; }
const DisplayTarget& DisplayTransaction::previousTarget() const { return m_PreviousTarget; }
const DisplayTarget& DisplayTransaction::requestedTarget() const { return m_RequestedTarget; }
const DisplayTarget& DisplayTransaction::verificationTarget() const
{
    return m_VerifyingRollback ? m_PreviousTarget : m_RequestedTarget;
}
quint64 DisplayTransaction::transactionEpoch() const { return m_TransactionEpoch; }
quint64 DisplayTransaction::sessionEpoch() const { return m_SessionEpoch; }
bool DisplayTransaction::active() const
{
    return m_Phase != DisplayPhase::Idle &&
        m_Phase != DisplayPhase::Succeeded && m_Phase != DisplayPhase::Failed;
}
bool DisplayTransaction::rollbackAttempted() const { return m_RollbackAttempted; }
bool DisplayTransaction::verifyingRollback() const { return m_VerifyingRollback; }
bool DisplayTransaction::requestedTargetSucceeded() const
{
    return m_Outcome == DisplayOutcome::RequestedTargetActive;
}
bool DisplayTransaction::hostMayHaveMutated() const { return m_HostMayHaveMutated; }
bool DisplayTransaction::sessionTokenMatches(const QString& token) const
{
    return !m_SessionToken.isEmpty() && m_SessionToken == token;
}
QString DisplayTransaction::failureReason() const { return m_FailureReason; }

bool DisplayTransaction::validTransactionEpoch(quint64 epoch) const
{
    return epoch != 0 && epoch == m_TransactionEpoch;
}

bool DisplayTransaction::validEvidenceEpoch(quint64 transactionEpoch,
                                            quint64 sessionEpoch) const
{
    return validTransactionEpoch(transactionEpoch) && sessionEpoch != 0 &&
        sessionEpoch == m_SessionEpoch;
}

bool DisplayTransaction::completeIfVerified()
{
    if (!m_FirstFrameDecoded || !m_ServerTargetVerified) {
        return false;
    }
    if (m_VerifyingRollback) {
        m_Outcome = DisplayOutcome::PreviousTargetRestored;
        setPhase(DisplayPhase::Failed);
    }
    else {
        m_Outcome = DisplayOutcome::RequestedTargetActive;
        setPhase(DisplayPhase::Succeeded);
    }
    m_SessionToken.clear();
    return true;
}

void DisplayTransaction::setPhase(DisplayPhase phase)
{
    if (m_Phase == phase) {
        return;
    }
    m_Phase = phase;
    m_PhaseHistory.push_back(phase);
}
