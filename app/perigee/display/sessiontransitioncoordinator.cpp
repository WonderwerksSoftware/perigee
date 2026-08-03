#include "sessiontransitioncoordinator.h"

#include "perigee/polaris/polarismodels.h"

#include <QTimer>

#include <algorithm>
#include <chrono>

namespace {

constexpr qint64 VerificationTimeoutMs = 15000;

QString targetKindLabel(const DisplayTarget& target)
{
    return target.kind == QStringLiteral("output")
        ? QStringLiteral("display") : QStringLiteral("stream mode");
}

}

SessionTransitionCoordinator::SessionTransitionCoordinator(Clock clock,
                                                           QObject* parent)
    : QObject(parent)
    , m_Clock(clock ? std::move(clock)
                    : Clock(&SessionTransitionCoordinator::monotonicMilliseconds))
{
    qRegisterMetaType<DisplayPhase>();
    auto timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this,
            &SessionTransitionCoordinator::checkDeadline);
    timer->start();
}

bool SessionTransitionCoordinator::attachSession(
    const DisplaySessionIdentity& identity,
    const std::shared_ptr<DisplayTransitionPort>& port)
{
    if (!port || identity.computerUuid.isEmpty() || identity.appId <= 0 ||
            identity.sessionEpoch == 0) {
        return false;
    }
    if (m_ExplicitResumePending) {
        if (!m_ReplacementPending ||
                !identityMatches(identity.computerUuid, identity.appId) ||
                identity.sessionEpoch == m_Identity.sessionEpoch) {
            return false;
        }
        m_Identity = identity;
        m_Port = port;
        m_ExplicitResumePending = false;
        m_ReplacementPending = false;
        setStatus(QStringLiteral("Connected to host"));
        emit stateChanged();
        return true;
    }
    if (m_Transaction.active()) {
        if (!identityMatches(identity.computerUuid, identity.appId)) {
            return false;
        }
        if (m_Transaction.phase() == DisplayPhase::Reconnecting) {
            if (!m_Transaction.onSessionAttached(m_Transaction.transactionEpoch(),
                                                 identity.sessionEpoch)) {
                return false;
            }
            m_Identity = identity;
            m_Port = port;
            m_ReplacementPending = false;
            enterVerification();
            emit stateChanged();
            return true;
        }
        if (identity.sessionEpoch != m_Identity.sessionEpoch) {
            return false;
        }
    }
    m_Identity = identity;
    m_Port = port;
    return true;
}

void SessionTransitionCoordinator::detachSession(quint64 sessionEpoch)
{
    if (sessionEpoch == 0 || sessionEpoch != m_Identity.sessionEpoch) {
        return;
    }
    m_Port.reset();
}

bool SessionTransitionCoordinator::selectTarget(
    const QVariantMap& authoritativeMetadata,
    const PolarisDiscoverySnapshot& discovery,
    std::function<void(const ActionResult&)> completion)
{
    return selectTarget(authoritativeMetadata,
                        DisplayTransaction::catalogIdentity(discovery),
                        discovery, std::move(completion));
}

bool SessionTransitionCoordinator::selectTarget(
    const QVariantMap& authoritativeMetadata,
    const QVariantMap& catalogIdentity,
    const PolarisDiscoverySnapshot& discovery,
    std::function<void(const ActionResult&)> completion)
{
    if (displaySelectionBusy()) {
        if (completion) {
            completion(failure(QStringLiteral("resource_busy"),
                               QStringLiteral("Another display selection is in progress.")));
        }
        return false;
    }
    const std::shared_ptr<DisplayTransitionPort> port = currentPort();
    if (!port) {
        if (completion) {
            completion(failure(QStringLiteral("session_retired"),
                               QStringLiteral("The streaming session is no longer active.")));
        }
        return false;
    }
    if (catalogIdentity.isEmpty() ||
            catalogIdentity != DisplayTransaction::catalogIdentity(discovery)) {
        if (completion) {
            completion(failure(QStringLiteral("stale_catalog"),
                               QStringLiteral("The display target catalog changed.")));
        }
        return false;
    }
    const PolarisAvailability availability = PolarisModels::availability(
        discovery, PolarisOperation::DisplaySwitch);
    if (!availability.enabled) {
        if (completion) {
            completion(failure(availability.errorCode, availability.reason));
        }
        return false;
    }

    const auto requestedIt = std::find_if(
        discovery.settings.targets.cbegin(), discovery.settings.targets.cend(),
        [&authoritativeMetadata](const DisplayTarget& candidate) {
            return targetMetadata(candidate) == authoritativeMetadata;
        });
    if (requestedIt == discovery.settings.targets.cend()) {
        if (completion) {
            completion(failure(QStringLiteral("stale_catalog"),
                               QStringLiteral("The display target catalog changed.")));
        }
        return false;
    }
    const DisplayTarget requested = *requestedIt;
    if (!requested.available || requested.current) {
        if (completion) {
            completion(failure(QStringLiteral("stale_catalog"),
                               QStringLiteral("The display target catalog changed.")));
        }
        return false;
    }
    if (discovery.settings.currentConflict) {
        if (completion) {
            completion(failure(QStringLiteral("current_target_conflict"),
                               QStringLiteral("Polaris reported conflicting current displays.")));
        }
        return false;
    }
    QVector<DisplayTarget> currentTargets;
    for (const DisplayTarget& candidate : discovery.settings.targets) {
        if (candidate.kind == requested.kind && candidate.current) {
            currentTargets.push_back(candidate);
        }
    }
    if (currentTargets.size() != 1) {
        if (completion) {
            completion(failure(QStringLiteral("current_target_missing"),
                               QStringLiteral("Polaris did not report one current display.")));
        }
        return false;
    }
    if (!identityMatches(m_Identity.computerUuid, m_Identity.appId)) {
        if (completion) {
            completion(failure(QStringLiteral("session_identity_mismatch"),
                               QStringLiteral("The streaming session changed.")));
        }
        return false;
    }

    const quint64 epoch = ++m_NextTransactionEpoch;
    if (!m_Transaction.begin(currentTargets.first(), requested,
                             discovery.session.sessionToken, epoch,
                             m_Identity.sessionEpoch)) {
        if (completion) {
            completion(failure(QStringLiteral("invalid_transition"),
                               QStringLiteral("The display transition could not start.")));
        }
        return false;
    }
    m_DeckNavigationOwnerSessionEpoch = m_Identity.sessionEpoch;
    m_ForceRollbackReconnect = false;
    m_Completion = std::move(completion);
    m_CompletionDelivered = false;
    m_RequestedMetadata = authoritativeMetadata;
    m_LastDiscovery = discovery;
    m_LastDiscoverySessionEpoch = m_Identity.sessionEpoch;
    m_VerifiedTarget.reset();
    m_RecoveryVisible = false;
    m_ReplacementPending = false;
    m_ExplicitResumePending = false;
    m_Deadline = -1;
    setStatus(QStringLiteral("Selecting %1").arg(requested.label));
    postRequestedTarget();
    emit stateChanged();
    return true;
}

void SessionTransitionCoordinator::observeDiscovery(
    quint64 sessionEpoch, const PolarisDiscoverySnapshot& discovery)
{
    if (!m_Transaction.active() ||
            m_Transaction.phase() != DisplayPhase::Verifying ||
            sessionEpoch != m_Transaction.sessionEpoch() ||
            !discovery.complete || discovery.standardHost ||
            !discovery.errorCode.isEmpty() || !discovery.session.valid ||
            !discovery.settings.valid || discovery.settings.currentConflict ||
            !m_Transaction.sessionTokenMatches(discovery.session.sessionToken)) {
        return;
    }
    if (sessionEpoch != m_VerificationSessionEpoch ||
            discovery.generation <= m_VerificationBaselineGeneration) {
        return;
    }
    m_LastDiscovery = discovery;
    m_LastDiscoverySessionEpoch = sessionEpoch;
    const DisplayTarget expected = m_Transaction.verificationTarget();
    const auto observed = std::find_if(
        discovery.settings.targets.cbegin(), discovery.settings.targets.cend(),
        [&expected](const DisplayTarget& candidate) {
            return candidate.kind == expected.kind && candidate.current;
        });
    if (observed == discovery.settings.targets.cend() ||
            observed->stableKey() != expected.stableKey()) {
        if (m_Transaction.verifyingRollback()) {
            m_Transaction.onRollbackFailed(m_Transaction.transactionEpoch(),
                                           QStringLiteral("target_mismatch"));
            m_RecoveryVisible = true;
            setStatus(QStringLiteral("The requested display and the previous display both failed."));
            finish(failure(QStringLiteral("rollback_failed"), m_StatusText));
        }
        else {
            postRollbackTarget(QStringLiteral("target_mismatch"));
        }
        emit stateChanged();
        return;
    }
    m_VerifiedTarget = *observed;
    m_Transaction.observeReadback(m_Transaction.transactionEpoch(),
                                  sessionEpoch, *observed);
    handleVerificationCompletion();
}

void SessionTransitionCoordinator::firstFrameDecoded(
    quint64 sessionEpoch, quint64 evidenceEpoch)
{
    if (!m_Transaction.active() ||
            m_Transaction.phase() != DisplayPhase::Verifying ||
            sessionEpoch != m_Transaction.sessionEpoch() ||
            evidenceEpoch == 0 || evidenceEpoch != m_CurrentEvidenceEpoch) {
        return;
    }
    m_Transaction.observeFirstFrame(m_Transaction.transactionEpoch(),
                                    sessionEpoch);
    handleVerificationCompletion();
}

void SessionTransitionCoordinator::sessionConnectionFailed(
    quint64 sessionEpoch, const QString& errorCode)
{
    if (!m_Transaction.active() ||
            m_Transaction.phase() != DisplayPhase::Verifying ||
            sessionEpoch != m_Transaction.sessionEpoch()) {
        return;
    }
    if (m_Transaction.verifyingRollback()) {
        m_Transaction.onRollbackFailed(
            m_Transaction.transactionEpoch(),
            errorCode.isEmpty() ? QStringLiteral("connection_failed")
                                : errorCode);
        m_RecoveryVisible = true;
        setStatus(QStringLiteral("The previous display could not be restored."));
        finish(failure(QStringLiteral("rollback_failed"), m_StatusText));
    }
    else {
        m_ForceRollbackReconnect = true;
        postRollbackTarget(errorCode.isEmpty()
                               ? QStringLiteral("connection_failed")
                               : errorCode);
    }
    emit stateChanged();
}

void SessionTransitionCoordinator::rollbackControlExpired(
    quint64 sessionEpoch)
{
    if (sessionEpoch != m_Transaction.sessionEpoch() ||
            m_Transaction.phase() != DisplayPhase::RollingBack ||
            !m_Transaction.onRollbackFailed(
                m_Transaction.transactionEpoch(),
                QStringLiteral("rollback_transport_timeout"))) {
        return;
    }
    m_RecoveryVisible = true;
    setStatus(QStringLiteral("The previous display could not be restored."));
    finish(failure(QStringLiteral("rollback_failed"), m_StatusText));
    emit stateChanged();
}

bool SessionTransitionCoordinator::sessionFinished(quint64 sessionEpoch)
{
    if (m_ExplicitResumePending && sessionEpoch == m_Identity.sessionEpoch) {
        m_ReplacementPending = true;
        emit replacementRequested();
        emit stateChanged();
        return true;
    }
    if (m_Transaction.active() &&
            m_Transaction.phase() == DisplayPhase::Disconnecting &&
            m_Transaction.onIntentionalDisconnect(
                m_Transaction.transactionEpoch(), sessionEpoch)) {
        m_ReplacementPending = true;
        setStatus(QStringLiteral("Reconnecting to verify the display"));
        emit replacementRequested();
        emit stateChanged();
        return true;
    }
    if (m_Transaction.active() &&
            m_Transaction.phase() == DisplayPhase::Reconnecting &&
            m_ReplacementPending &&
            sessionEpoch == m_Identity.sessionEpoch) {
        return true;
    }
    if (m_Transaction.active() && sessionEpoch == m_Identity.sessionEpoch) {
        cancelForUnexpectedDisconnect();
    }
    return false;
}

void SessionTransitionCoordinator::checkDeadline()
{
    if (!m_Transaction.active() || m_Deadline < 0 ||
            m_Transaction.phase() != DisplayPhase::Verifying ||
            m_Clock() < m_Deadline) {
        return;
    }
    m_Deadline = -1;
    if (m_Transaction.verifyingRollback()) {
        m_Transaction.onRollbackFailed(m_Transaction.transactionEpoch(),
                                       QStringLiteral("verification_timeout"));
        m_RecoveryVisible = true;
        setStatus(QStringLiteral("The previous display could not be restored."));
        finish(failure(QStringLiteral("rollback_failed"), m_StatusText));
    }
    else {
        postRollbackTarget(QStringLiteral("verification_timeout"));
    }
    emit stateChanged();
}

DisplayPhase SessionTransitionCoordinator::phase() const { return m_Transaction.phase(); }
QString SessionTransitionCoordinator::statusText() const { return m_StatusText; }
bool SessionTransitionCoordinator::recoveryVisible() const { return m_RecoveryVisible; }
bool SessionTransitionCoordinator::replacementPending() const { return m_ReplacementPending; }
bool SessionTransitionCoordinator::displaySelectionBusy() const
{
    return m_Transaction.active();
}
bool SessionTransitionCoordinator::verificationPending() const
{
    return m_Transaction.phase() == DisplayPhase::Verifying;
}
quint64 SessionTransitionCoordinator::transactionEpoch() const
{
    return m_Transaction.transactionEpoch();
}

bool SessionTransitionCoordinator::identityMatches(const QString& computerUuid,
                                                   int appId) const
{
    return !computerUuid.isEmpty() && computerUuid == m_Identity.computerUuid &&
        appId > 0 && appId == m_Identity.appId;
}

QString SessionTransitionCoordinator::targetActionId(
    const DisplayTarget& target) const
{
    const QByteArray encoded = target.stableKey().toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QStringLiteral("display.target.%1").arg(QString::fromLatin1(encoded));
}

ActionState SessionTransitionCoordinator::decorateDisplaySelectionState(
    ActionState state) const
{
    if (displaySelectionBusy()) {
        state.enabled = false;
        state.phase = ActionPhase::Working;
        state.disabledCode = QStringLiteral("resource_busy");
        state.disabledReason = QStringLiteral("Another display selection is in progress.");
        state.message = m_StatusText;
    }
    return state;
}

ActionState SessionTransitionCoordinator::decorateTargetState(
    const DisplayTarget& target, ActionState state) const
{
    state = decorateDisplaySelectionState(std::move(state));
    if (target.current) {
        state.enabled = false;
        state.disabledCode = QStringLiteral("current_target");
        state.disabledReason = QStringLiteral("This display is currently active.");
    }
    return state;
}

void SessionTransitionCoordinator::setDeckNavigationState(
    quint64 sessionEpoch,
    const DeckDisplayNavigationState& state)
{
    if (sessionEpoch == 0) {
        return;
    }
    if (m_Transaction.active() || m_DeckRestorePending) {
        if (sessionEpoch != m_DeckNavigationOwnerSessionEpoch) {
            return;
        }
    }
    else if (sessionEpoch != m_Identity.sessionEpoch) {
        return;
    }
    m_DeckNavigation = state;
}

bool SessionTransitionCoordinator::takeDeckRestore(quint64 sessionEpoch,
                                                   QString* actionId)
{
    if (!m_DeckRestorePending || sessionEpoch != m_Transaction.sessionEpoch() ||
            !m_Transaction.requestedTargetSucceeded()) {
        return false;
    }
    m_DeckRestorePending = false;
    m_DeckNavigationOwnerSessionEpoch = sessionEpoch;
    if (actionId) {
        *actionId = targetActionId(m_Transaction.requestedTarget());
    }
    return m_DeckNavigation.deckWasOpen && !m_DeckNavigation.explicitlyClosed;
}

void SessionTransitionCoordinator::retry()
{
    if (!m_RecoveryVisible || m_RequestedMetadata.isEmpty() ||
            !currentPort()) {
        return;
    }
    const auto completion = std::move(m_Completion);
    m_Completion = {};
    m_RecoveryVisible = false;
    m_ExplicitRetryInProgress = true;
    if (!selectTarget(m_RequestedMetadata, m_LastDiscovery,
                      std::move(completion))) {
        m_ExplicitRetryInProgress = false;
        m_RecoveryVisible = true;
        setStatus(QStringLiteral("The display retry could not start."));
        emit stateChanged();
    }
}

void SessionTransitionCoordinator::disconnect()
{
    if (!m_RecoveryVisible) {
        return;
    }
    const std::shared_ptr<DisplayTransitionPort> port = currentPort();
    m_RecoveryVisible = false;
    m_ExplicitResumePending = false;
    m_ExplicitRetryInProgress = false;
    clearCompletion();
    if (port) {
        port->requestLocalDisconnect(0);
    }
    emit normalDisconnectRequested();
    emit stateChanged();
}

void SessionTransitionCoordinator::returnToHost()
{
    if (!m_RecoveryVisible) {
        return;
    }
    const std::shared_ptr<DisplayTransitionPort> port = currentPort();
    m_RecoveryVisible = false;
    m_ExplicitResumePending = true;
    m_ExplicitRetryInProgress = false;
    clearCompletion();
    if (port) {
        port->requestLocalDisconnect(0);
    }
    else {
        m_ReplacementPending = true;
        emit replacementRequested();
    }
    emit stateChanged();
}

QVariantMap SessionTransitionCoordinator::targetMetadata(
    const DisplayTarget& target)
{
    return {
        {QStringLiteral("kind"), target.kind},
        {QStringLiteral("id"), target.id},
        {QStringLiteral("label"), target.label},
        {QStringLiteral("available"), target.available},
        {QStringLiteral("current"), target.current},
        {QStringLiteral("requires_reconnect"), target.requiresReconnect},
        {QStringLiteral("unavailable_reason"), target.unavailableReason},
    };
}

ActionResult SessionTransitionCoordinator::failure(const QString& code,
                                                   const QString& message)
{
    return {false, {}, code, message};
}

qint64 SessionTransitionCoordinator::monotonicMilliseconds()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::shared_ptr<DisplayTransitionPort>
SessionTransitionCoordinator::currentPort() const
{
    return m_Port.lock();
}

void SessionTransitionCoordinator::postRequestedTarget()
{
    const std::shared_ptr<DisplayTransitionPort> port = currentPort();
    const quint64 epoch = m_Transaction.transactionEpoch();
    if (!port) {
        m_Transaction.onPostRejected(epoch, QStringLiteral("session_retired"));
        finish(failure(QStringLiteral("session_retired"),
                       QStringLiteral("The streaming session is no longer active.")));
        return;
    }
    const QString token = m_LastDiscovery.session.sessionToken;
    port->postAuthorizedTarget(
        m_Transaction.requestedTarget(), token, epoch,
        m_LastDiscovery,
        [this](quint64 deliveredEpoch, bool accepted,
               const QString& errorCode, const QString& userMessage) {
            handlePostCompletion(deliveredEpoch, false, accepted,
                                 errorCode, userMessage);
        });
}

void SessionTransitionCoordinator::postRollbackTarget(const QString& reason)
{
    const quint64 epoch = m_Transaction.transactionEpoch();
    if (!m_Transaction.beginRollback(epoch, reason)) {
        return;
    }
    m_Deadline = -1;
    setStatus(QStringLiteral("Restoring the previous %1")
                  .arg(targetKindLabel(m_Transaction.previousTarget())));
    const std::shared_ptr<DisplayTransitionPort> port = currentPort();
    if (!port) {
        m_Transaction.onRollbackFailed(epoch, QStringLiteral("session_retired"));
        m_RecoveryVisible = true;
        finish(failure(QStringLiteral("rollback_failed"),
                       QStringLiteral("The previous display could not be restored.")));
        return;
    }
    port->postAuthorizedTarget(
        m_Transaction.previousTarget(), m_LastDiscovery.session.sessionToken,
        epoch,
        m_LastDiscovery,
        [this](quint64 deliveredEpoch, bool accepted,
               const QString& errorCode, const QString& userMessage) {
            handlePostCompletion(deliveredEpoch, true, accepted,
                                 errorCode, userMessage);
        });
}

void SessionTransitionCoordinator::handlePostCompletion(
    quint64 epoch, bool rollback, bool accepted,
    const QString& errorCode, const QString& userMessage)
{
    if (epoch == 0 || epoch != m_Transaction.transactionEpoch() ||
            (!rollback && m_Transaction.phase() != DisplayPhase::Selecting) ||
            (rollback && m_Transaction.phase() != DisplayPhase::RollingBack)) {
        return;
    }
    if (!accepted) {
        if (rollback) {
            m_Transaction.onRollbackFailed(
                epoch, errorCode.isEmpty() ? QStringLiteral("post_failed") : errorCode);
            m_RecoveryVisible = true;
            setStatus(userMessage.isEmpty()
                          ? QStringLiteral("The previous display could not be restored.")
                          : userMessage);
            finish(failure(QStringLiteral("rollback_failed"), m_StatusText));
        }
        else {
            m_Transaction.onPostRejected(
                epoch, errorCode.isEmpty() ? QStringLiteral("post_failed") : errorCode);
            setStatus(userMessage.isEmpty()
                          ? QStringLiteral("Polaris rejected the display change.")
                          : userMessage);
            finish(failure(errorCode.isEmpty() ? QStringLiteral("post_failed") : errorCode,
                           m_StatusText));
        }
        emit stateChanged();
        return;
    }
    const bool advanced = rollback
        ? m_Transaction.onRollbackPostAccepted(
              epoch, m_ForceRollbackReconnect)
        : m_Transaction.onPostAccepted(epoch);
    if (!advanced) {
        return;
    }
    if (rollback) {
        m_ForceRollbackReconnect = false;
    }
    if (m_Transaction.phase() == DisplayPhase::Disconnecting) {
        requestDisconnectOrFail(rollback);
    }
    else {
        enterVerification();
    }
    emit stateChanged();
}

void SessionTransitionCoordinator::enterVerification()
{
    m_VerificationSessionEpoch = m_Transaction.sessionEpoch();
    m_VerificationBaselineGeneration =
        m_LastDiscoverySessionEpoch == m_VerificationSessionEpoch
            ? m_LastDiscovery.generation : 0;
    m_Deadline = m_Clock() + VerificationTimeoutMs;
    if (m_NextEvidenceEpoch >=
            FirstFrameNotificationGate::MaximumEvidenceEpoch) {
        m_NextEvidenceEpoch = 1;
    }
    else {
        ++m_NextEvidenceEpoch;
    }
    m_CurrentEvidenceEpoch = m_NextEvidenceEpoch;
    setStatus(m_Transaction.verifyingRollback()
                  ? QStringLiteral("Verifying the restored display")
                  : QStringLiteral("Verifying the selected display"));
    if (const std::shared_ptr<DisplayTransitionPort> port = currentPort()) {
        port->armFirstFrameEvidence(m_CurrentEvidenceEpoch);
        port->refreshReadback(m_Transaction.transactionEpoch());
    }
}

void SessionTransitionCoordinator::requestDisconnectOrFail(bool rollback)
{
    const std::shared_ptr<DisplayTransitionPort> port = currentPort();
    if (port && port->requestLocalDisconnect(m_Transaction.transactionEpoch())) {
        if (m_Transaction.phase() == DisplayPhase::Disconnecting) {
            setStatus(QStringLiteral(
                "Disconnecting locally for the display change"));
        }
        return;
    }
    if (rollback) {
        m_Transaction.onRollbackFailed(m_Transaction.transactionEpoch(),
                                       QStringLiteral("disconnect_failed"));
        m_RecoveryVisible = true;
        finish(failure(QStringLiteral("rollback_failed"),
                       QStringLiteral("The rollback reconnect could not start.")));
    }
    else {
        postRollbackTarget(QStringLiteral("disconnect_failed"));
    }
}

void SessionTransitionCoordinator::handleVerificationCompletion()
{
    if (m_Transaction.phase() == DisplayPhase::Succeeded) {
        m_Deadline = -1;
        m_DeckRestorePending = true;
        setStatus(QStringLiteral("Display changed to %1")
                      .arg(m_Transaction.requestedTarget().label));
        ActionState observed;
        observed.enabled = false;
        DisplayTarget verified = m_VerifiedTarget.value_or(
            m_Transaction.requestedTarget());
        verified.current = true;
        observed.value = targetMetadata(verified);
        observed.invocationState =
            DisplayTransaction::catalogIdentity(m_LastDiscovery);
        observed.disabledCode = QStringLiteral("current_target");
        observed.disabledReason = QStringLiteral("This display is currently active.");
        ActionResult result {true, QStringLiteral("Display verified"), {}, {}};
        result.observedState = observed;
        finish(std::move(result));
        emit stateChanged();
    }
    else if (m_Transaction.phase() == DisplayPhase::Failed &&
             m_Transaction.outcome() == DisplayOutcome::PreviousTargetRestored) {
        m_Deadline = -1;
        setStatus(QStringLiteral("The selected display failed. The previous display was restored."));
        finish(failure(QStringLiteral("switch_failed_restored"), m_StatusText));
        emit stateChanged();
    }
}

void SessionTransitionCoordinator::finish(ActionResult result)
{
    if (m_CompletionDelivered) {
        return;
    }
    m_CompletionDelivered = true;
    if (m_Transaction.phase() == DisplayPhase::Succeeded ||
            m_Transaction.phase() == DisplayPhase::Failed) {
        m_ForceRollbackReconnect = false;
    }
    if (m_ExplicitRetryInProgress) {
        m_ExplicitRetryInProgress = false;
        if (!result.ok) {
            m_RecoveryVisible = true;
        }
    }
    auto completion = std::move(m_Completion);
    m_Completion = {};
    if (completion) {
        completion(result);
    }
}

void SessionTransitionCoordinator::clearCompletion()
{
    m_Completion = {};
    m_CompletionDelivered = true;
}

void SessionTransitionCoordinator::setStatus(const QString& text)
{
    m_StatusText = text;
}

void SessionTransitionCoordinator::cancelForUnexpectedDisconnect()
{
    const quint64 epoch = m_Transaction.transactionEpoch();
    if (m_Transaction.cancelBeforeMutation(epoch) ||
            m_Transaction.cancelUnexpectedDisconnect(m_Identity.sessionEpoch)) {
        m_Deadline = -1;
        m_ReplacementPending = false;
        setStatus(QStringLiteral("Display change cancelled by disconnect."));
        finish(failure(QStringLiteral("cancelled"), m_StatusText));
        emit stateChanged();
    }
}
