#include "test_registry.h"

#include "streaming/sessionexitintent.h"

#include <QtTest>

class SessionExitIntentTest : public QObject
{
    Q_OBJECT

private slots:
    void disconnectKeepsPerigeeAndHostRunning();
    void legacyDisconnectStillHonorsHostPreference();
    void quitPerigeeKeepsHostRunning();
    void rejectedDisconnectDoesNotLeaveStaleKeepHostIntent();
    void rejectedQuitDoesNotLeaveStaleExitIntent();
    void legacyQuitHostAndExitOverridesDisconnectIntent();
    void ordinaryPerigeeExitStillHonorsHostPreference();
    void unexpectedTerminationNeverQuitsHost();
};

void SessionExitIntentTest::disconnectKeepsPerigeeAndHostRunning()
{
    SessionExitIntent intent;

    QVERIFY(SessionRequest::disconnectClient(
        intent,
        ClientDisconnectPolicy::KeepHostRunning,
        [] { return 1; }));

    QVERIFY(!intent.shouldExitPerigee());
    QVERIFY(!intent.shouldQuitHost(false, true));
}

void SessionExitIntentTest::legacyDisconnectStillHonorsHostPreference()
{
    SessionExitIntent intent;

    QVERIFY(SessionRequest::disconnectClient(
        intent,
        ClientDisconnectPolicy::HonorHostQuitPreference,
        [] { return 1; }));

    QVERIFY(!intent.shouldExitPerigee());
    QVERIFY(intent.shouldQuitHost(false, true));
    QVERIFY(!intent.shouldQuitHost(false, false));
}

void SessionExitIntentTest::quitPerigeeKeepsHostRunning()
{
    SessionExitIntent intent;

    QVERIFY(SessionRequest::quitPerigee(intent, [] { return 1; }));

    QVERIFY(intent.shouldExitPerigee());
    QVERIFY(!intent.shouldQuitHost(false, true));
}

void SessionExitIntentTest::rejectedDisconnectDoesNotLeaveStaleKeepHostIntent()
{
    SessionExitIntent intent;

    QVERIFY(!SessionRequest::disconnectClient(
        intent,
        ClientDisconnectPolicy::KeepHostRunning,
        [] { return -1; }));
    intent.requestPerigeeExit(false);

    QVERIFY(intent.shouldExitPerigee());
    QVERIFY(intent.shouldQuitHost(false, true));
}

void SessionExitIntentTest::rejectedQuitDoesNotLeaveStaleExitIntent()
{
    SessionExitIntent intent;

    QVERIFY(!SessionRequest::quitPerigee(intent, [] { return 0; }));
    QVERIFY(!intent.shouldExitPerigee());
    intent.requestPerigeeExit(false);

    QVERIFY(intent.shouldQuitHost(false, true));
}

void SessionExitIntentTest::legacyQuitHostAndExitOverridesDisconnectIntent()
{
    SessionExitIntent intent;

    intent.requestClientDisconnect();
    intent.requestPerigeeExit(true);

    QVERIFY(intent.shouldExitPerigee());
    QVERIFY(intent.shouldQuitHost(false, true));
    QVERIFY(intent.shouldQuitHost(false, false));
}

void SessionExitIntentTest::ordinaryPerigeeExitStillHonorsHostPreference()
{
    SessionExitIntent intent;

    intent.requestPerigeeExit(false);

    QVERIFY(intent.shouldExitPerigee());
    QVERIFY(intent.shouldQuitHost(false, true));
    QVERIFY(!intent.shouldQuitHost(false, false));
}

void SessionExitIntentTest::unexpectedTerminationNeverQuitsHost()
{
    SessionExitIntent intent;

    intent.requestPerigeeExit(true);

    QVERIFY(!intent.shouldQuitHost(true, true));
    QVERIFY(!intent.shouldQuitHost(true, false));
}

REGISTER_PERIGEE_TEST(SessionExitIntentTest);

#include "test_sessionexitintent.moc"
