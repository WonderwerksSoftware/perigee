#include "test_registry.h"

#include "streaming/sessionexitintent.h"

#include <QtTest>

class SessionExitIntentTest : public QObject
{
    Q_OBJECT

private slots:
    void disconnectKeepsPerigeeAndHostRunning();
    void quitPerigeeKeepsHostRunning();
    void legacyQuitHostAndExitOverridesDisconnectIntent();
    void ordinaryPerigeeExitStillHonorsHostPreference();
    void unexpectedTerminationNeverQuitsHost();
};

void SessionExitIntentTest::disconnectKeepsPerigeeAndHostRunning()
{
    SessionExitIntent intent;

    intent.requestClientDisconnect();

    QVERIFY(!intent.shouldExitPerigee());
    QVERIFY(!intent.shouldQuitHost(false, true));
}

void SessionExitIntentTest::quitPerigeeKeepsHostRunning()
{
    SessionExitIntent intent;

    intent.requestPerigeeQuit();

    QVERIFY(intent.shouldExitPerigee());
    QVERIFY(!intent.shouldQuitHost(false, true));
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
}

REGISTER_PERIGEE_TEST(SessionExitIntentTest);

#include "test_sessionexitintent.moc"
