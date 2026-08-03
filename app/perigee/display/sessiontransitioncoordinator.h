#pragma once

#include "displaytransaction.h"
#include "perigee/actions/actiontypes.h"

#include <QObject>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>

struct DisplaySessionIdentity
{
    QString computerUuid;
    int appId = 0;
    QString appName;
    quint64 sessionEpoch = 0;
};

struct DeckDisplayNavigationState
{
    bool deckWasOpen = false;
    bool explicitlyClosed = false;
    QString focusedActionId;
};

class DisplayTransitionPort
{
public:
    using PostCompletion = std::function<void(
        quint64 transactionEpoch, bool accepted,
        const QString& errorCode, const QString& userMessage)>;

    virtual ~DisplayTransitionPort() = default;
    virtual void postTarget(const DisplayTarget& target,
                            const QString& sessionToken,
                            quint64 transactionEpoch,
                            PostCompletion completion) = 0;
    virtual void postAuthorizedTarget(
        const DisplayTarget& target, const QString& sessionToken,
        quint64 transactionEpoch,
        const PolarisDiscoverySnapshot& authorizationSnapshot,
        PostCompletion completion)
    {
        Q_UNUSED(authorizationSnapshot);
        postTarget(target, sessionToken, transactionEpoch,
                   std::move(completion));
    }
    virtual bool requestLocalDisconnect(quint64 transactionEpoch) = 0;
    virtual void armFirstFrameEvidence(quint64 evidenceEpoch)
    {
        Q_UNUSED(evidenceEpoch);
    }
    virtual void refreshReadback(quint64 transactionEpoch) = 0;
};

class SessionTransitionCoordinator final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(DisplayPhase phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool recoveryVisible READ recoveryVisible NOTIFY stateChanged)
    Q_PROPERTY(bool replacementPending READ replacementPending NOTIFY stateChanged)
    Q_PROPERTY(bool displaySelectionBusy READ displaySelectionBusy NOTIFY stateChanged)
    Q_PROPERTY(bool verificationPending READ verificationPending NOTIFY stateChanged)

public:
    using Clock = std::function<qint64()>;

    explicit SessionTransitionCoordinator(Clock clock = {},
                                          QObject* parent = nullptr);

    bool attachSession(const DisplaySessionIdentity& identity,
                       const std::shared_ptr<DisplayTransitionPort>& port);
    void detachSession(quint64 sessionEpoch);

    bool selectTarget(const QVariantMap& authoritativeMetadata,
                      const PolarisDiscoverySnapshot& discovery,
                      std::function<void(const ActionResult&)> completion);
    bool selectTarget(const QVariantMap& authoritativeMetadata,
                      const QVariantMap& catalogIdentity,
                      const PolarisDiscoverySnapshot& discovery,
                      std::function<void(const ActionResult&)> completion);
    void observeDiscovery(quint64 sessionEpoch,
                          const PolarisDiscoverySnapshot& discovery);
    void firstFrameDecoded(quint64 sessionEpoch, quint64 evidenceEpoch);
    void sessionConnectionFailed(quint64 sessionEpoch,
                                 const QString& errorCode);
    void rollbackControlExpired(quint64 sessionEpoch);
    bool sessionFinished(quint64 sessionEpoch);
    void checkDeadline();

    DisplayPhase phase() const;
    QString statusText() const;
    bool recoveryVisible() const;
    bool replacementPending() const;
    bool displaySelectionBusy() const;
    bool verificationPending() const;
    quint64 transactionEpoch() const;
    bool identityMatches(const QString& computerUuid, int appId) const;
    QString targetActionId(const DisplayTarget& target) const;
    ActionState decorateDisplaySelectionState(ActionState state) const;
    ActionState decorateTargetState(const DisplayTarget& target,
                                    ActionState state) const;

    void setDeckNavigationState(quint64 sessionEpoch,
                                const DeckDisplayNavigationState& state);
    bool takeDeckRestore(quint64 sessionEpoch, QString* actionId);

    Q_INVOKABLE void retry();
    Q_INVOKABLE void disconnect();
    Q_INVOKABLE void returnToHost();

signals:
    void stateChanged();
    void replacementRequested();
    void normalDisconnectRequested();

private:
    static QVariantMap targetMetadata(const DisplayTarget& target);
    static ActionResult failure(const QString& code, const QString& message);
    static qint64 monotonicMilliseconds();

    std::shared_ptr<DisplayTransitionPort> currentPort() const;
    void postRequestedTarget();
    void postRollbackTarget(const QString& reason);
    void handlePostCompletion(quint64 epoch, bool rollback,
                              bool accepted, const QString& errorCode,
                              const QString& userMessage);
    void enterVerification();
    void requestDisconnectOrFail(bool rollback);
    void handleVerificationCompletion();
    void finish(ActionResult result);
    void clearCompletion();
    void setStatus(const QString& text);
    void cancelForUnexpectedDisconnect();

    Clock m_Clock;
    DisplayTransaction m_Transaction;
    DisplaySessionIdentity m_Identity;
    std::weak_ptr<DisplayTransitionPort> m_Port;
    std::function<void(const ActionResult&)> m_Completion;
    QVariantMap m_RequestedMetadata;
    PolarisDiscoverySnapshot m_LastDiscovery;
    std::optional<DisplayTarget> m_VerifiedTarget;
    quint64 m_LastDiscoverySessionEpoch = 0;
    quint64 m_VerificationBaselineGeneration = 0;
    quint64 m_VerificationSessionEpoch = 0;
    quint64 m_NextEvidenceEpoch = 0;
    quint64 m_CurrentEvidenceEpoch = 0;
    DeckDisplayNavigationState m_DeckNavigation;
    quint64 m_DeckNavigationOwnerSessionEpoch = 0;
    quint64 m_NextTransactionEpoch = 0;
    qint64 m_Deadline = -1;
    QString m_StatusText;
    bool m_RecoveryVisible = false;
    bool m_ReplacementPending = false;
    bool m_CompletionDelivered = false;
    bool m_DeckRestorePending = false;
    bool m_ExplicitResumePending = false;
    bool m_ExplicitRetryInProgress = false;
    bool m_ForceRollbackReconnect = false;
};

Q_DECLARE_METATYPE(DisplayPhase)
