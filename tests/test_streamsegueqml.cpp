#include "test_registry.h"

#include <QFile>
#include <QFileInfo>
#include <QtTest>

class StreamSegueQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void rollbackFailureHasThreeExplicitFocusSafeControllerActions();
    void transitionRetiresOldSessionBeforeCreatingReplacement();
    void recoveryUsesAFreshDormantSessionAsItsControlCarrier();
    void recoveryActionsWaitForAUsableDormantCarrier();
};

void StreamSegueQmlTest::rollbackFailureHasThreeExplicitFocusSafeControllerActions()
{
    const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
    QFile file(tests.filePath(QStringLiteral("../app/gui/StreamSegue.qml")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray qml = file.readAll();

    QVERIFY(qml.contains("objectName: \"displayRecoverySurface\""));
    QVERIFY(qml.contains("text: qsTr(\"Retry\")"));
    QVERIFY(qml.contains("text: qsTr(\"Disconnect\")"));
    QVERIFY(qml.contains("text: qsTr(\"Return to host\")"));
    QVERIFY(qml.contains("DisplayTransitionCoordinator.retry()"));
    QVERIFY(qml.contains("DisplayTransitionCoordinator.disconnect()"));
    QVERIFY(qml.contains("DisplayTransitionCoordinator.returnToHost()"));
    QCOMPARE(qml.count(
        "enabled: displayRecoverySurface.visible && recoveryActionsReady"), 3);
    QCOMPARE(qml.count(
        "activeFocusOnTab: displayRecoverySurface.visible && recoveryActionsReady"), 3);
}

void StreamSegueQmlTest::recoveryActionsWaitForAUsableDormantCarrier()
{
    const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
    QFile file(tests.filePath(QStringLiteral("../app/gui/StreamSegue.qml")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray qml = file.readAll();

    QVERIFY(qml.contains(
        "property bool recoveryActionsReady: recoveryCarrier && session !== null"));
    QVERIFY(qml.contains("onRecoveryActionsReadyChanged:"));
    QVERIFY(qml.contains(
        "if (recoveryActionsReady && displayRecoverySurface.visible)"));
    QVERIFY(qml.contains("retryButton.forceActiveFocus()"));
}

void StreamSegueQmlTest::transitionRetiresOldSessionBeforeCreatingReplacement()
{
    const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
    QFile file(tests.filePath(QStringLiteral("../app/gui/StreamSegue.qml")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray qml = file.readAll();

    QVERIFY(qml.contains("session.displayTransitionHandoff"));
    QVERIFY(qml.contains("DisplayTransitionCoordinator.replacementPending"));
    QVERIFY(qml.contains("retiredSession.createDisplayTransitionReplacement()"));
    const int ready = qml.indexOf("function sessionReadyForDeletion()");
    const int clearOld = qml.indexOf("session = null", ready);
    const int collectOld = qml.indexOf("gc()", clearOld);
    const int installReplacement = qml.indexOf("session = replacement", collectOld);
    QVERIFY(ready >= 0);
    QVERIFY(clearOld > ready);
    QVERIFY(collectOld > clearOld);
    QVERIFY(installReplacement > collectOld);
}

void StreamSegueQmlTest::recoveryUsesAFreshDormantSessionAsItsControlCarrier()
{
    const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
    QFile file(tests.filePath(QStringLiteral("../app/gui/StreamSegue.qml")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray qml = file.readAll();

    QVERIFY(qml.contains("property bool recoveryCarrier: false"));
    QVERIFY(qml.contains("replaceCleanedSession(false)"));
    QVERIFY(qml.contains("session.pumpDisplayTransitionControl()"));
    QVERIFY(qml.contains("DisplayTransitionCoordinator.verificationPending"));
    QVERIFY(qml.contains("startRecoveryCarrier"));
    QVERIFY(qml.contains("retiredSession.disposeDormantDisplayTransitionCarrier()"));

    const int replace = qml.indexOf("function replaceCleanedSession");
    const int dispose = qml.indexOf(
        "retiredSession.disposeDormantDisplayTransitionCarrier()", replace);
    const int clear = qml.indexOf("session = null", replace);
    QVERIFY(replace >= 0);
    QVERIFY(dispose > replace);
    QVERIFY(clear > dispose);
}

REGISTER_PERIGEE_TEST(StreamSegueQmlTest);

#include "test_streamsegueqml.moc"
