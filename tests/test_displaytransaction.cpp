#include "perigee/display/displaytransaction.h"
#include "test_registry.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {

DisplayTarget target(QString kind, QString id, bool reconnect,
                     bool current = false)
{
    DisplayTarget result;
    result.kind = std::move(kind);
    result.id = std::move(id);
    result.label = result.id;
    result.available = true;
    result.current = current;
    result.requiresReconnect = reconnect;
    return result;
}

}

class DisplayTransactionTest : public QObject
{
    Q_OBJECT

private slots:
    void reconnectHappyPathRequiresReadbackAndCurrentEpochFrame();
    void inPlaceHappyPathSkipsDisconnect();
    void postBodiesUseTargetFieldAndSessionToken();
    void staleEvidenceCannotCompleteNewTransaction();
    void rollbackIsAttemptedOnlyOnceAndRestorationIsNotSwitchSuccess();
    void cancellationAndFirstFrameGateFailClosed();
    void catalogIdentityTracksGenerationAndOrderWithoutSessionToken();
};

REGISTER_PERIGEE_TEST(DisplayTransactionTest);

void DisplayTransactionTest::reconnectHappyPathRequiresReadbackAndCurrentEpochFrame()
{
    DisplayTransaction transaction;
    const DisplayTarget previous = target("output", "DP-1", true, true);
    const DisplayTarget requested = target("output", "DP-2", true);

    QVERIFY(transaction.begin(previous, requested, QStringLiteral("secret"),
                              11, 40));
    QCOMPARE(transaction.phaseHistory(),
             QVector<DisplayPhase>({DisplayPhase::Idle,
                                    DisplayPhase::Selecting}));
    QVERIFY(transaction.onPostAccepted(11));
    QCOMPARE(transaction.phase(), DisplayPhase::Disconnecting);
    QVERIFY(transaction.onIntentionalDisconnect(11, 40));
    QCOMPARE(transaction.phase(), DisplayPhase::Reconnecting);
    QVERIFY(transaction.onSessionAttached(11, 41));
    QCOMPARE(transaction.phase(), DisplayPhase::Verifying);

    DisplayTarget readback = requested;
    readback.current = true;
    QVERIFY(transaction.observeReadback(11, 41, readback));
    QCOMPARE(transaction.phase(), DisplayPhase::Verifying);
    QVERIFY(!transaction.observeFirstFrame(10, 41));
    QVERIFY(!transaction.observeFirstFrame(11, 40));
    QCOMPARE(transaction.phase(), DisplayPhase::Verifying);
    QVERIFY(transaction.observeFirstFrame(11, 41));
    QCOMPARE(transaction.phase(), DisplayPhase::Succeeded);
    QCOMPARE(transaction.outcome(), DisplayOutcome::RequestedTargetActive);
    QCOMPARE(transaction.phaseHistory(),
             QVector<DisplayPhase>({DisplayPhase::Idle,
                                    DisplayPhase::Selecting,
                                    DisplayPhase::Disconnecting,
                                    DisplayPhase::Reconnecting,
                                    DisplayPhase::Verifying,
                                    DisplayPhase::Succeeded}));
}

void DisplayTransactionTest::inPlaceHappyPathSkipsDisconnect()
{
    DisplayTransaction transaction;
    const DisplayTarget previous = target("stream-mode", "desktop_display", false, true);
    const DisplayTarget requested = target("stream-mode", "virtual_display", false);
    QVERIFY(transaction.begin(previous, requested, QStringLiteral("token"), 7, 3));
    QVERIFY(transaction.onPostAccepted(7));
    QCOMPARE(transaction.phase(), DisplayPhase::Verifying);
    QVERIFY(transaction.observeFirstFrame(7, 3));
    QCOMPARE(transaction.phase(), DisplayPhase::Verifying);
    DisplayTarget readback = requested;
    readback.current = true;
    QVERIFY(transaction.observeReadback(7, 3, readback));
    QCOMPARE(transaction.phase(), DisplayPhase::Succeeded);
    QCOMPARE(transaction.phaseHistory(),
             QVector<DisplayPhase>({DisplayPhase::Idle,
                                    DisplayPhase::Selecting,
                                    DisplayPhase::Verifying,
                                    DisplayPhase::Succeeded}));
}

void DisplayTransactionTest::postBodiesUseTargetFieldAndSessionToken()
{
    const QByteArray outputBody = DisplayTransaction::postBody(
        target("output", "DP-3", true), QStringLiteral("current-token"));
    const QByteArray modeBody = DisplayTransaction::postBody(
        target("stream-mode", "headless", true), QStringLiteral("current-token"));

    const QJsonObject output = QJsonDocument::fromJson(outputBody).object();
    QCOMPARE(output.size(), 2);
    QCOMPARE(output.value("output_name").toString(), QStringLiteral("DP-3"));
    QCOMPARE(output.value("session_token").toString(), QStringLiteral("current-token"));
    QVERIFY(!outputBody.contains('\n'));
    QVERIFY(!outputBody.contains("  "));

    const QJsonObject mode = QJsonDocument::fromJson(modeBody).object();
    QCOMPARE(mode.size(), 2);
    QCOMPARE(mode.value("stream_display_mode").toString(), QStringLiteral("headless"));
    QCOMPARE(mode.value("session_token").toString(), QStringLiteral("current-token"));
    QVERIFY(DisplayTransaction::postBody(target("unknown", "x", false),
                                         QStringLiteral("token")).isEmpty());
}

void DisplayTransactionTest::catalogIdentityTracksGenerationAndOrderWithoutSessionToken()
{
    PolarisDiscoverySnapshot first;
    first.generation = 9;
    first.session.sessionToken = QStringLiteral("DO_NOT_PUBLISH_CATALOG_TOKEN");
    first.settings.targets = {
        target("output", "DP-1", false, true),
        target("output", "DP-2", false),
    };
    PolarisDiscoverySnapshot reordered = first;
    std::swap(reordered.settings.targets[0], reordered.settings.targets[1]);
    PolarisDiscoverySnapshot newer = first;
    newer.generation = 10;

    const QVariantMap firstIdentity =
        DisplayTransaction::catalogIdentity(first);
    QVERIFY(!firstIdentity.isEmpty());
    QVERIFY(firstIdentity != DisplayTransaction::catalogIdentity(reordered));
    QVERIFY(firstIdentity != DisplayTransaction::catalogIdentity(newer));
    const QByteArray serializedIdentity =
        QJsonDocument::fromVariant(firstIdentity).toJson(
            QJsonDocument::Compact);
    QVERIFY(!serializedIdentity.contains("DO_NOT_PUBLISH_CATALOG_TOKEN"));
}

void DisplayTransactionTest::staleEvidenceCannotCompleteNewTransaction()
{
    DisplayTransaction transaction;
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    QVERIFY(transaction.begin(previous, requested, QStringLiteral("token"), 1, 10));
    QVERIFY(transaction.onPostAccepted(1));
    QVERIFY(transaction.cancelBeforeMutation(1) == false);
    QVERIFY(transaction.beginRollback(1, QStringLiteral("mismatch")));
    QVERIFY(transaction.onRollbackPostAccepted(1));
    QVERIFY(transaction.observeReadback(1, 10, previous));
    QVERIFY(transaction.observeFirstFrame(1, 10));
    QCOMPARE(transaction.outcome(), DisplayOutcome::PreviousTargetRestored);

    QVERIFY(transaction.begin(previous, requested, QStringLiteral("new-token"), 2, 20));
    QVERIFY(transaction.onPostAccepted(2));
    QVERIFY(!transaction.observeReadback(1, 10, requested));
    QVERIFY(!transaction.observeFirstFrame(1, 10));
    QVERIFY(!transaction.observeReadback(2, 19, requested));
    QVERIFY(!transaction.observeFirstFrame(2, 19));
    QCOMPARE(transaction.phase(), DisplayPhase::Verifying);
}

void DisplayTransactionTest::rollbackIsAttemptedOnlyOnceAndRestorationIsNotSwitchSuccess()
{
    DisplayTransaction transaction;
    const DisplayTarget previous = target("output", "DP-1", true, true);
    const DisplayTarget requested = target("output", "DP-2", true);
    QVERIFY(transaction.begin(previous, requested, QStringLiteral("token"), 9, 4));
    QVERIFY(transaction.onPostAccepted(9));
    QVERIFY(transaction.beginRollback(9, QStringLiteral("reconnect_failed")));
    QCOMPARE(transaction.phase(), DisplayPhase::RollingBack);
    QVERIFY(transaction.rollbackAttempted());
    QVERIFY(!transaction.beginRollback(9, QStringLiteral("again")));
    QVERIFY(transaction.onRollbackPostAccepted(9));
    QCOMPARE(transaction.phase(), DisplayPhase::Disconnecting);
    QVERIFY(transaction.onIntentionalDisconnect(9, 4));
    QVERIFY(transaction.onSessionAttached(9, 5));
    QVERIFY(transaction.observeReadback(9, 5, previous));
    QVERIFY(transaction.observeFirstFrame(9, 5));
    QCOMPARE(transaction.phase(), DisplayPhase::Failed);
    QCOMPARE(transaction.outcome(), DisplayOutcome::PreviousTargetRestored);
    QVERIFY(!transaction.requestedTargetSucceeded());

    QVERIFY(transaction.begin(previous, requested, QStringLiteral("token"), 10, 6));
    QVERIFY(transaction.onPostAccepted(10));
    QVERIFY(transaction.beginRollback(10, QStringLiteral("timeout")));
    QVERIFY(transaction.onRollbackFailed(10, QStringLiteral("post_failed")));
    QCOMPARE(transaction.outcome(), DisplayOutcome::RollbackFailed);
    QVERIFY(!transaction.beginRollback(10, QStringLiteral("loop")));
}

void DisplayTransactionTest::cancellationAndFirstFrameGateFailClosed()
{
    DisplayTransaction transaction;
    const DisplayTarget previous = target("output", "DP-1", false, true);
    const DisplayTarget requested = target("output", "DP-2", false);
    QVERIFY(transaction.begin(previous, requested, QStringLiteral("token"), 3, 1));
    QVERIFY(transaction.cancelBeforeMutation(3));
    QCOMPARE(transaction.phase(), DisplayPhase::Idle);
    QCOMPARE(transaction.outcome(), DisplayOutcome::Cancelled);

    FirstFrameNotificationGate gate;
    int enqueueCalls = 0;
    quint64 queuedEvidenceEpoch = 0;

    // A normal decoded frame before a display verification is armed must not
    // consume the one-shot notification for a later in-place switch.
    QVERIFY(!gate.notifyAcceptedFrame([&](quint64) {
        ++enqueueCalls;
        return true;
    }));
    QCOMPARE(enqueueCalls, 0);

    gate.arm(100);
    QVERIFY(!gate.notifyAcceptedFrame([&](quint64 evidenceEpoch) {
        ++enqueueCalls;
        queuedEvidenceEpoch = evidenceEpoch;
        return false;
    }));
    QCOMPARE(gate.queuedEvidenceEpoch(), quint64(0));
    QVERIFY(gate.notifyAcceptedFrame([&](quint64 evidenceEpoch) {
        ++enqueueCalls;
        queuedEvidenceEpoch = evidenceEpoch;
        return true;
    }));
    QCOMPARE(queuedEvidenceEpoch, quint64(100));
    QCOMPARE(gate.queuedEvidenceEpoch(), quint64(100));
    QVERIFY(!gate.notifyAcceptedFrame([&](quint64) {
        ++enqueueCalls;
        return true;
    }));
    QCOMPARE(enqueueCalls, 2);

    // A new verification attempt gets a fresh one-shot even if an SDL event
    // for the prior attempt remains queued; that old event carries epoch 100.
    gate.arm(101);
    QVERIFY(gate.notifyAcceptedFrame([&](quint64 evidenceEpoch) {
        ++enqueueCalls;
        queuedEvidenceEpoch = evidenceEpoch;
        return true;
    }));
    QCOMPARE(queuedEvidenceEpoch, quint64(101));
    QCOMPARE(enqueueCalls, 3);

    gate.disarm();
    QCOMPARE(gate.armedEvidenceEpoch(), quint64(0));
    QCOMPARE(gate.queuedEvidenceEpoch(), quint64(0));
}

#include "test_displaytransaction.moc"
