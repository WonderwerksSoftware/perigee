#include "perigee/display/sessiontransitioncoordinator.h"
#include "test_registry.h"

#include <QtTest>

#include <memory>

namespace {

DisplayTarget target(QString kind, QString id, bool reconnect,
                     bool current = false, bool available = true)
{
    DisplayTarget value;
    value.kind = std::move(kind);
    value.id = std::move(id);
    value.label = value.id;
    value.requiresReconnect = reconnect;
    value.current = current;
    value.available = available;
    return value;
}

PolarisDiscoverySnapshot discovery(const DisplayTarget& previous,
                                   const DisplayTarget& requested)
{
    PolarisDiscoverySnapshot snapshot;
    snapshot.generation = 1;
    snapshot.complete = true;
    snapshot.capabilities.valid = true;
    snapshot.capabilities.isPolarisContract = true;
    snapshot.capabilities.features.insert(QStringLiteral("display_targets_v1"));
    snapshot.capabilities.clientSettingsEndpoint.usable = true;
    snapshot.capabilities.clientSettingsEndpoint.advertised =
        QStringLiteral("/polaris/v1/client-settings");
    snapshot.session.valid = true;
    snapshot.session.state = QStringLiteral("streaming");
    snapshot.session.sessionToken = QStringLiteral("do-not-publish-this-token");
    snapshot.session.tokenValid = true;
    snapshot.session.role = QStringLiteral("owner");
    snapshot.session.controllingClient = true;
    snapshot.session.ownsSession = true;
    snapshot.session.controls.displaySelectionAllowed = true;
    snapshot.settings.valid = true;
    snapshot.settings.targets = {previous, requested};
    return snapshot;
}

QVariantMap metadata(const DisplayTarget& value)
{
    return {
        {QStringLiteral("kind"), value.kind},
        {QStringLiteral("id"), value.id},
        {QStringLiteral("label"), value.label},
        {QStringLiteral("available"), value.available},
        {QStringLiteral("current"), value.current},
        {QStringLiteral("requires_reconnect"), value.requiresReconnect},
        {QStringLiteral("unavailable_reason"), value.unavailableReason},
    };
}

class FakePort final : public DisplayTransitionPort
{
public:
    struct Post {
        DisplayTarget target;
        QString token;
        quint64 transactionEpoch = 0;
        PostCompletion completion;
    };

    void postTarget(const DisplayTarget& value, const QString& token,
                    quint64 epoch, PostCompletion completion) override
    {
        posts.push_back({value, token, epoch, std::move(completion)});
    }

    bool requestLocalDisconnect(quint64 transactionEpoch) override
    {
        disconnectEpochs.push_back(transactionEpoch);
        if (disconnectHook) {
            return disconnectHook(transactionEpoch);
        }
        return disconnectResult;
    }

    void refreshReadback(quint64 transactionEpoch) override
    {
        refreshEpochs.push_back(transactionEpoch);
    }

    void armFirstFrameEvidence(quint64 evidenceEpoch) override
    {
        armedEvidenceEpochs.push_back(evidenceEpoch);
    }

    QVector<Post> posts;
    QVector<quint64> disconnectEpochs;
    QVector<quint64> refreshEpochs;
    QVector<quint64> armedEvidenceEpochs;
    std::function<bool(quint64)> disconnectHook;
    bool disconnectResult = true;
};

DisplaySessionIdentity identity(quint64 sessionEpoch)
{
    DisplaySessionIdentity value;
    value.computerUuid = QStringLiteral("computer-uuid");
    value.appId = 42;
    value.appName = QStringLiteral("Desktop");
    value.sessionEpoch = sessionEpoch;
    return value;
}

}

class SessionTransitionCoordinatorTest : public QObject
{
    Q_OBJECT

private slots:
    void authenticatedReconnectCompletesOnlyAfterNewEpochEvidence();
    void catalogAndCurrentTargetRevalidationFailClosed();
    void resourceStaysBusyAcrossPortRetirement();
    void replacementConnectionFailureEntersRollback();
    void stalePublishedGenerationCannotTriggerRollback();
    void rollbackVerificationRequiresPostRollbackGeneration();
    void sequentialInPlaceSwitchesRejectStaleSameSessionFrame();
    void duplicateIntentionalFinishDuringFailedReplacementIsIdempotent();
    void timeoutRollsBackExactlyOnceAndFailureRequiresExplicitRecovery();
    void explicitRetryFailureAlwaysRestoresRecoverySurface();
    void returnToHostReplacementIsConsumedExactlyOnce();
    void userDisconnectAndForeignSessionCannotStartReplacement();
    void completionIsAtMostOnceAndDoesNotRetainPort();
    void deckRestoreStateIsOwnedByOriginalSession();
    void reorderedCatalogIsRejectedBeforePost();
    void retryClearsForcedRollbackReconnectState();
};

REGISTER_PERIGEE_TEST(SessionTransitionCoordinatorTest);

void SessionTransitionCoordinatorTest::authenticatedReconnectCompletesOnlyAfterNewEpochEvidence()
{
    SessionTransitionCoordinator coordinator;
    auto firstPort = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(10), firstPort));
    const DisplayTarget previous = target("output", "DP-1", true, true);
    const DisplayTarget requested = target("output", "DP-2", true);
    ActionResult result;
    int completions = 0;

    QVERIFY(coordinator.selectTarget(
        metadata(requested), discovery(previous, requested),
        [&](const ActionResult& value) { result = value; ++completions; }));
    QCOMPARE(firstPort->posts.size(), 1);
    QCOMPARE(firstPort->posts.first().target.stableKey(), requested.stableKey());
    QCOMPARE(firstPort->posts.first().token,
             QStringLiteral("do-not-publish-this-token"));
    QCOMPARE(completions, 0);

    firstPort->posts.first().completion(firstPort->posts.first().transactionEpoch,
                                        true, {}, {});
    QCOMPARE(firstPort->disconnectEpochs.size(), 1);
    QCOMPARE(coordinator.phase(), DisplayPhase::Disconnecting);
    QVERIFY(coordinator.sessionFinished(10));
    QCOMPARE(coordinator.phase(), DisplayPhase::Reconnecting);
    coordinator.detachSession(10);

    auto replacement = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(11), replacement));
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QCOMPARE(replacement->refreshEpochs.size(), 1);
    QCOMPARE(replacement->armedEvidenceEpochs.size(), 1);
    PolarisDiscoverySnapshot readback = discovery(requested, previous);
    readback.generation = 1;
    readback.settings.targets[0].current = true;
    readback.settings.targets[1].current = false;
    coordinator.observeDiscovery(10, readback);
    coordinator.firstFrameDecoded(10, replacement->armedEvidenceEpochs.last());
    QCOMPARE(completions, 0);
    coordinator.observeDiscovery(11, readback);
    QCOMPARE(completions, 0);
    coordinator.firstFrameDecoded(11, replacement->armedEvidenceEpochs.last());
    QCOMPARE(completions, 1);
    QVERIFY(result.ok);
    QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
    QVERIFY(!coordinator.statusText().contains(QStringLiteral("do-not-publish")));
}

void SessionTransitionCoordinatorTest::catalogAndCurrentTargetRevalidationFailClosed()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    ActionResult result;

    QVariantMap stale = metadata(requested);
    stale[QStringLiteral("label")] = QStringLiteral("old label");
    QVERIFY(!coordinator.selectTarget(stale, discovery(previous, requested),
                                      [&](const ActionResult& value) { result = value; }));
    QCOMPARE(result.errorCode, QStringLiteral("stale_catalog"));
    QVERIFY(port->posts.isEmpty());

    PolarisDiscoverySnapshot conflict = discovery(previous, requested);
    conflict.settings.currentConflict = true;
    QVERIFY(!coordinator.selectTarget(metadata(requested), conflict,
                                      [&](const ActionResult& value) { result = value; }));
    QCOMPARE(result.errorCode, QStringLiteral("current_target_conflict"));
    QVERIFY(port->posts.isEmpty());

    PolarisDiscoverySnapshot missing = discovery(previous, requested);
    missing.settings.targets[0].current = false;
    QVERIFY(!coordinator.selectTarget(metadata(requested), missing,
                                      [&](const ActionResult& value) { result = value; }));
    QCOMPARE(result.errorCode, QStringLiteral("current_target_missing"));
    QVERIFY(port->posts.isEmpty());
}

void SessionTransitionCoordinatorTest::reorderedCatalogIsRejectedBeforePost()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    const PolarisDiscoverySnapshot published = discovery(previous, requested);
    PolarisDiscoverySnapshot reordered = published;
    std::swap(reordered.settings.targets[0], reordered.settings.targets[1]);
    ActionResult result;

    QVERIFY(!coordinator.selectTarget(
        metadata(requested), DisplayTransaction::catalogIdentity(published),
        reordered, [&](const ActionResult& value) { result = value; }));
    QCOMPARE(result.errorCode, QStringLiteral("stale_catalog"));
    QVERIFY(port->posts.isEmpty());
}

void SessionTransitionCoordinatorTest::deckRestoreStateIsOwnedByOriginalSession()
{
    const auto completeReconnect = [](SessionTransitionCoordinator& coordinator,
                                      const std::shared_ptr<FakePort>& original,
                                      const DisplayTarget& previous,
                                      const DisplayTarget& requested,
                                      quint64 replacementEpoch) {
        original->posts[0].completion(
            original->posts[0].transactionEpoch, true, {}, {});
        QVERIFY(coordinator.sessionFinished(1));
        coordinator.detachSession(1);
        auto replacement = std::make_shared<FakePort>();
        QVERIFY(coordinator.attachSession(identity(replacementEpoch), replacement));
        PolarisDiscoverySnapshot readback = discovery(requested, previous);
        readback.generation = 2;
        readback.settings.targets[0].current = true;
        readback.settings.targets[1].current = false;
        coordinator.observeDiscovery(replacementEpoch, readback);
        coordinator.firstFrameDecoded(
            replacementEpoch, replacement->armedEvidenceEpochs.last());
        QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
    };

    const DisplayTarget previous = target("output", "DP-1", true, true);
    const DisplayTarget requested = target("output", "DP-2", true);
    SessionTransitionCoordinator openCoordinator;
    auto openPort = std::make_shared<FakePort>();
    QVERIFY(openCoordinator.attachSession(identity(1), openPort));
    openCoordinator.setDeckNavigationState(
        1, {true, false, QStringLiteral("display.target.old")});
    const PolarisDiscoverySnapshot initial = discovery(previous, requested);
    QVERIFY(openCoordinator.selectTarget(
        metadata(requested), DisplayTransaction::catalogIdentity(initial),
        initial, {}));
    completeReconnect(openCoordinator, openPort, previous, requested, 2);
    openCoordinator.setDeckNavigationState(2, {false, false, {}});
    QString restoredAction;
    QVERIFY(openCoordinator.takeDeckRestore(2, &restoredAction));
    QCOMPARE(restoredAction,
             openCoordinator.targetActionId(requested));

    SessionTransitionCoordinator closedCoordinator;
    auto closedPort = std::make_shared<FakePort>();
    QVERIFY(closedCoordinator.attachSession(identity(1), closedPort));
    closedCoordinator.setDeckNavigationState(1, {true, false, {}});
    QVERIFY(closedCoordinator.selectTarget(
        metadata(requested), DisplayTransaction::catalogIdentity(initial),
        initial, {}));
    closedCoordinator.setDeckNavigationState(1, {false, true, {}});
    completeReconnect(closedCoordinator, closedPort, previous, requested, 2);
    closedCoordinator.setDeckNavigationState(2, {true, false, {}});
    QVERIFY(!closedCoordinator.takeDeckRestore(2, nullptr));
}

void SessionTransitionCoordinatorTest::retryClearsForcedRollbackReconnectState()
{
    SessionTransitionCoordinator coordinator;
    auto initialPort = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), initialPort));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", true);
    const PolarisDiscoverySnapshot initial = discovery(previous, requested);
    QVERIFY(coordinator.selectTarget(
        metadata(requested), DisplayTransaction::catalogIdentity(initial),
        initial, {}));
    initialPort->posts[0].completion(
        initialPort->posts[0].transactionEpoch, true, {}, {});
    QVERIFY(coordinator.sessionFinished(1));
    coordinator.detachSession(1);

    auto failedReplacement = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(2), failedReplacement));
    coordinator.sessionConnectionFailed(2, QStringLiteral("connection_failed"));
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    failedReplacement->posts[0].completion(
        failedReplacement->posts[0].transactionEpoch, false,
        QStringLiteral("post_failed"), {});
    QVERIFY(coordinator.recoveryVisible());

    coordinator.retry();
    QCOMPARE(failedReplacement->posts.size(), 2);
    failedReplacement->posts[1].completion(
        failedReplacement->posts[1].transactionEpoch, true, {}, {});
    QVERIFY(coordinator.sessionFinished(2));
    coordinator.detachSession(2);
    auto retryReplacement = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(3), retryReplacement));

    PolarisDiscoverySnapshot mismatch = initial;
    mismatch.generation = 2;
    coordinator.observeDiscovery(3, mismatch);
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    QCOMPARE(retryReplacement->posts.size(), 1);
    retryReplacement->posts[0].completion(
        retryReplacement->posts[0].transactionEpoch, true, {}, {});
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QVERIFY(retryReplacement->disconnectEpochs.isEmpty());
}

void SessionTransitionCoordinatorTest::resourceStaysBusyAcrossPortRetirement()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", true, true);
    const DisplayTarget requested = target("output", "DP-2", true);
    ActionResult second;

    QVERIFY(coordinator.selectTarget(metadata(requested), discovery(previous, requested), {}));
    QVERIFY(coordinator.displaySelectionBusy());
    coordinator.detachSession(1);
    QVERIFY(coordinator.displaySelectionBusy());
    QVERIFY(!coordinator.selectTarget(metadata(requested), discovery(previous, requested),
                                      [&](const ActionResult& value) { second = value; }));
    QCOMPARE(second.errorCode, QStringLiteral("resource_busy"));
}

void SessionTransitionCoordinatorTest::replacementConnectionFailureEntersRollback()
{
    SessionTransitionCoordinator coordinator;
    auto first = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), first));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", true);
    QVERIFY(coordinator.selectTarget(metadata(requested),
                                     discovery(previous, requested), {}));
    first->posts[0].completion(first->posts[0].transactionEpoch, true, {}, {});
    QVERIFY(coordinator.sessionFinished(1));
    coordinator.detachSession(1);

    auto replacement = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(2), replacement));
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    coordinator.sessionConnectionFailed(2, QStringLiteral("connection_failed"));
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    QCOMPARE(replacement->posts.size(), 1);
    QCOMPARE(replacement->posts[0].target.stableKey(), previous.stableKey());
    replacement->posts[0].completion(
        replacement->posts[0].transactionEpoch, true, {}, {});
    QCOMPARE(coordinator.phase(), DisplayPhase::Disconnecting);
}

void SessionTransitionCoordinatorTest::stalePublishedGenerationCannotTriggerRollback()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    const PolarisDiscoverySnapshot initial = discovery(previous, requested);

    QVERIFY(coordinator.selectTarget(metadata(requested), initial, {}));
    port->posts[0].completion(port->posts[0].transactionEpoch, true, {}, {});
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);

    // PolarisAdapter keeps publishing the previous complete generation while
    // the requested refresh is in flight. It is not mismatch evidence.
    coordinator.observeDiscovery(1, initial);
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QCOMPARE(port->posts.size(), 1);

    PolarisDiscoverySnapshot staleMatching = discovery(requested, previous);
    staleMatching.generation = initial.generation;
    staleMatching.settings.targets[0].current = true;
    staleMatching.settings.targets[1].current = false;
    coordinator.observeDiscovery(1, staleMatching);
    coordinator.firstFrameDecoded(1, port->armedEvidenceEpochs.last());
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);

    PolarisDiscoverySnapshot fresh = staleMatching;
    fresh.generation = initial.generation + 1;
    coordinator.observeDiscovery(1, fresh);
    QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
}

void SessionTransitionCoordinatorTest::rollbackVerificationRequiresPostRollbackGeneration()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    const PolarisDiscoverySnapshot initial = discovery(previous, requested);

    QVERIFY(coordinator.selectTarget(metadata(requested), initial, {}));
    port->posts[0].completion(port->posts[0].transactionEpoch, true, {}, {});

    PolarisDiscoverySnapshot mismatch = initial;
    mismatch.generation = initial.generation + 1;
    coordinator.observeDiscovery(1, mismatch);
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    QCOMPARE(port->posts.size(), 2);
    port->posts[1].completion(port->posts[1].transactionEpoch, true, {}, {});
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QCOMPARE(port->armedEvidenceEpochs.size(), 2);

    // This snapshot matches the rollback target but predates rollback POST
    // acceptance, so it cannot count as restoration evidence.
    coordinator.observeDiscovery(1, mismatch);
    coordinator.firstFrameDecoded(1, port->armedEvidenceEpochs.first());
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);

    PolarisDiscoverySnapshot restored = mismatch;
    restored.generation = mismatch.generation + 1;
    coordinator.observeDiscovery(1, restored);
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    coordinator.firstFrameDecoded(1, port->armedEvidenceEpochs.last());
    QCOMPARE(coordinator.phase(), DisplayPhase::Failed);
}

void SessionTransitionCoordinatorTest::sequentialInPlaceSwitchesRejectStaleSameSessionFrame()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget first = target("output", "DP-1", false, true);
    const DisplayTarget second = target("output", "DP-2", false);
    const DisplayTarget third = target("output", "DP-3", false);
    ActionResult firstResult;

    // A normal connection frame has no armed evidence epoch. The first
    // in-place verification arms its own one-shot after POST acceptance.
    QVERIFY(coordinator.selectTarget(
        metadata(second), discovery(first, second),
        [&](const ActionResult& result) { firstResult = result; }));
    port->posts[0].completion(port->posts[0].transactionEpoch, true, {}, {});
    QCOMPARE(port->armedEvidenceEpochs.size(), 1);
    const quint64 firstEvidenceEpoch = port->armedEvidenceEpochs.last();

    DisplayTarget currentSecond = second;
    currentSecond.current = true;
    DisplayTarget inactiveFirst = first;
    inactiveFirst.current = false;
    PolarisDiscoverySnapshot secondActive = discovery(currentSecond, inactiveFirst);
    secondActive.generation = 2;
    coordinator.observeDiscovery(1, secondActive);
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    coordinator.firstFrameDecoded(1, firstEvidenceEpoch);
    QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
    QVERIFY(firstResult.ok);
    QVERIFY(firstResult.observedState.has_value());
    QCOMPARE(firstResult.observedState->disabledCode,
             QStringLiteral("current_target"));
    QVERIFY(firstResult.observedState->value.toMap()
                .value(QStringLiteral("current")).toBool());

    PolarisDiscoverySnapshot next = discovery(currentSecond, third);
    next.generation = 3;
    QVERIFY(coordinator.selectTarget(metadata(third), next, {}));
    port->posts[1].completion(port->posts[1].transactionEpoch, true, {}, {});
    QCOMPARE(port->armedEvidenceEpochs.size(), 2);
    const quint64 secondEvidenceEpoch = port->armedEvidenceEpochs.last();
    QVERIFY(secondEvidenceEpoch > firstEvidenceEpoch);

    DisplayTarget currentThird = third;
    currentThird.current = true;
    DisplayTarget inactiveSecond = second;
    PolarisDiscoverySnapshot thirdActive = discovery(currentThird, inactiveSecond);
    thirdActive.generation = 4;
    coordinator.observeDiscovery(1, thirdActive);
    coordinator.firstFrameDecoded(1, firstEvidenceEpoch);
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    coordinator.firstFrameDecoded(1, secondEvidenceEpoch);
    QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
}

void SessionTransitionCoordinatorTest::duplicateIntentionalFinishDuringFailedReplacementIsIdempotent()
{
    SessionTransitionCoordinator coordinator;
    auto initial = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), initial));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", true);

    QVERIFY(coordinator.selectTarget(metadata(requested),
                                     discovery(previous, requested), {}));
    initial->posts[0].completion(initial->posts[0].transactionEpoch,
                                 true, {}, {});
    QVERIFY(coordinator.sessionFinished(1));
    coordinator.detachSession(1);

    auto replacement = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(2), replacement));
    coordinator.sessionConnectionFailed(2, QStringLiteral("initialization_failed"));
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    QCOMPARE(replacement->posts.size(), 1);

    replacement->disconnectHook = [&](quint64) {
        // A dormant replacement port reports its accepted rollback disconnect
        // synchronously from requestLocalDisconnect().
        return coordinator.sessionFinished(2);
    };
    replacement->posts[0].completion(
        replacement->posts[0].transactionEpoch, true, {}, {});
    QCOMPARE(coordinator.phase(), DisplayPhase::Reconnecting);
    QVERIFY(coordinator.replacementPending());
    QVERIFY(coordinator.statusText().contains(QStringLiteral("Reconnecting")));

    // Session::prepareDisplayTransitionHandoff() may observe the same finished
    // carrier. It must preserve the already-scheduled rollback replacement.
    QVERIFY(coordinator.sessionFinished(2));
    QCOMPARE(coordinator.phase(), DisplayPhase::Reconnecting);
    QVERIFY(coordinator.replacementPending());
    QVERIFY(coordinator.displaySelectionBusy());
}

void SessionTransitionCoordinatorTest::timeoutRollsBackExactlyOnceAndFailureRequiresExplicitRecovery()
{
    qint64 now = 100;
    SessionTransitionCoordinator coordinator([&]() { return now; });
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("stream-mode", "desktop_display", false, true);
    const DisplayTarget requested = target("stream-mode", "headless", false);
    QVERIFY(coordinator.selectTarget(metadata(requested), discovery(previous, requested), {}));
    port->posts[0].completion(port->posts[0].transactionEpoch, true, {}, {});
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    now += 14999;
    coordinator.checkDeadline();
    QCOMPARE(port->posts.size(), 1);
    ++now;
    coordinator.checkDeadline();
    QCOMPARE(port->posts.size(), 2);
    QCOMPARE(port->posts[1].target.stableKey(), previous.stableKey());
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    port->posts[1].completion(port->posts[1].transactionEpoch, false,
                              QStringLiteral("post_failed"),
                              QStringLiteral("Rollback failed"));
    QVERIFY(coordinator.recoveryVisible());
    QCOMPARE(coordinator.phase(), DisplayPhase::Failed);
    now += 60000;
    coordinator.checkDeadline();
    QCOMPARE(port->posts.size(), 2);

    coordinator.retry();
    QCOMPARE(port->posts.size(), 3);
    QCOMPARE(port->posts[2].target.stableKey(), requested.stableKey());
}

void SessionTransitionCoordinatorTest::explicitRetryFailureAlwaysRestoresRecoverySurface()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);

    QVERIFY(coordinator.selectTarget(metadata(requested),
                                     discovery(previous, requested), {}));
    port->posts[0].completion(port->posts[0].transactionEpoch, true, {}, {});
    PolarisDiscoverySnapshot stale = discovery(previous, requested);
    stale.generation = 2;
    stale.settings.targets = {previous};
    coordinator.observeDiscovery(1, stale);
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    port->posts[1].completion(port->posts[1].transactionEpoch, false,
                              QStringLiteral("post_failed"), {});
    QVERIFY(coordinator.recoveryVisible());

    const int postsBeforeSynchronousRetry = port->posts.size();
    coordinator.retry();
    QCOMPARE(port->posts.size(), postsBeforeSynchronousRetry);
    QVERIFY(coordinator.recoveryVisible());

    // Restore a valid carried catalog so retry reaches the asynchronous POST.
    PolarisDiscoverySnapshot valid = discovery(previous, requested);
    // A matching readback updates the carried snapshot only during verification,
    // so begin a fresh coordinator recovery with the valid snapshot instead.
    SessionTransitionCoordinator postFailure;
    auto postFailurePort = std::make_shared<FakePort>();
    QVERIFY(postFailure.attachSession(identity(1), postFailurePort));
    QVERIFY(postFailure.selectTarget(metadata(requested), valid, {}));
    postFailurePort->posts[0].completion(
        postFailurePort->posts[0].transactionEpoch, true, {}, {});
    postFailure.checkDeadline();
    // Force rollback directly through a target mismatch while retaining both
    // catalog entries in the carried snapshot.
    PolarisDiscoverySnapshot mismatch = valid;
    mismatch.generation = valid.generation + 1;
    mismatch.settings.targets[0].current = true;
    mismatch.settings.targets[1].current = false;
    postFailure.observeDiscovery(1, mismatch);
    postFailurePort->posts[1].completion(
        postFailurePort->posts[1].transactionEpoch, false,
        QStringLiteral("post_failed"), {});
    QVERIFY(postFailure.recoveryVisible());
    postFailure.retry();
    QVERIFY(!postFailure.recoveryVisible());
    QCOMPARE(postFailurePort->posts.size(), 3);
    postFailurePort->posts[2].completion(
        postFailurePort->posts[2].transactionEpoch, false,
        QStringLiteral("post_failed"), QStringLiteral("Retry rejected"));
    QVERIFY(postFailure.recoveryVisible());

    SessionTransitionCoordinator initialFailure;
    auto initialPort = std::make_shared<FakePort>();
    QVERIFY(initialFailure.attachSession(identity(1), initialPort));
    QVERIFY(initialFailure.selectTarget(metadata(requested), valid, {}));
    initialPort->posts[0].completion(initialPort->posts[0].transactionEpoch,
                                     false, QStringLiteral("post_failed"), {});
    QVERIFY(!initialFailure.recoveryVisible());
}

void SessionTransitionCoordinatorTest::returnToHostReplacementIsConsumedExactlyOnce()
{
    SessionTransitionCoordinator coordinator;
    auto carrier = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), carrier));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    QVERIFY(coordinator.selectTarget(metadata(requested),
                                     discovery(previous, requested), {}));
    carrier->posts[0].completion(carrier->posts[0].transactionEpoch, true, {}, {});
    PolarisDiscoverySnapshot mismatch = discovery(previous, requested);
    mismatch.generation = 2;
    coordinator.observeDiscovery(1, mismatch);
    carrier->posts[1].completion(carrier->posts[1].transactionEpoch, false,
                                 QStringLiteral("post_failed"), {});
    QVERIFY(coordinator.recoveryVisible());

    coordinator.returnToHost();
    QVERIFY(coordinator.sessionFinished(1));
    QVERIFY(coordinator.replacementPending());
    coordinator.detachSession(1);

    auto resumed = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(2), resumed));
    QVERIFY(!coordinator.replacementPending());
    QVERIFY(!coordinator.sessionFinished(2));
    QVERIFY(!coordinator.replacementPending());
}

void SessionTransitionCoordinatorTest::userDisconnectAndForeignSessionCannotStartReplacement()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    QVERIFY(coordinator.selectTarget(metadata(requested), discovery(previous, requested), {}));

    QVERIFY(!coordinator.sessionFinished(1));
    QVERIFY(!coordinator.displaySelectionBusy());
    QCOMPARE(coordinator.phase(), DisplayPhase::Idle);

    QVERIFY(coordinator.attachSession(identity(2), port));
    QVERIFY(coordinator.selectTarget(metadata(requested), discovery(previous, requested), {}));
    port->posts.last().completion(port->posts.last().transactionEpoch, true, {}, {});
    DisplaySessionIdentity foreign = identity(3);
    foreign.computerUuid = QStringLiteral("other-computer");
    auto foreignPort = std::make_shared<FakePort>();
    QVERIFY(!coordinator.attachSession(foreign, foreignPort));
}

void SessionTransitionCoordinatorTest::completionIsAtMostOnceAndDoesNotRetainPort()
{
    SessionTransitionCoordinator coordinator;
    auto port = std::make_shared<FakePort>();
    std::weak_ptr<FakePort> weakPort = port;
    QVERIFY(coordinator.attachSession(identity(1), port));
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    int completions = 0;
    QVERIFY(coordinator.selectTarget(metadata(requested), discovery(previous, requested),
                                     [&](const ActionResult&) { ++completions; }));
    const auto callback = port->posts[0].completion;
    const quint64 epoch = port->posts[0].transactionEpoch;
    coordinator.detachSession(1);
    port.reset();
    QVERIFY(weakPort.expired());
    callback(epoch, false, QStringLiteral("post_failed"), QStringLiteral("failed"));
    callback(epoch, true, {}, {});
    QCOMPARE(completions, 1);
}

#include "test_sessiontransitioncoordinator.moc"
