#include "test_registry.h"

#include <QFile>
#include <QFileInfo>
#include <QtTest>

namespace {

QByteArray sourceFile(const QString& relative)
{
    const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
    QFile file(tests.filePath(QStringLiteral("../") + relative));
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Unable to read source contract file: %s", qPrintable(file.fileName()));
    }
    return file.readAll();
}

}

class SessionDisplayTransitionTest : public QObject
{
    Q_OBJECT

private slots:
    void firstFrameEventIsOneShotEpochScopedAndHandledOnSdlThread();
    void firstFrameGatePublishesArmAndQueueAsOneAtomicState();
    void sdlLoopPumpsDeadlineAndReplacementFailure();
    void dormantRecoverySessionCanDriveControlBeforeStartingStream();
    void recoveryControlDoesNotDependOnDeckRendererAvailability();
    void deckNavigationUpdatesAreSessionEpochScopedAndRestoreUsesNonActivatingFocus();
    void openingControllerSelectsLayoutBeforeFirstDeckFrame();
};

void SessionDisplayTransitionTest::firstFrameEventIsOneShotEpochScopedAndHandledOnSdlThread()
{
    const QByteArray codes = sourceFile(QStringLiteral("app/streaming/sdleventcodes.h"));
    const QByteArray session = sourceFile(QStringLiteral("app/streaming/session.cpp"));
    QVERIFY(codes.contains("SDL_CODE_PERIGEE_FIRST_FRAME"));
    QVERIFY(session.contains("m_FirstFrameNotificationGate.notifyAcceptedFrame"));
    QVERIFY(session.contains("SDL_CODE_PERIGEE_FIRST_FRAME"));
    QVERIFY(session.contains("armFirstFrameEvidence"));
    QVERIFY(session.contains("firstFrameDecoded(\n"
                             "                        m_DisplaySessionEpoch, evidenceEpoch)"));
    QVERIFY(session.contains("event.user.data1"));
    QVERIFY(session.contains("event.user.data2"));
    QVERIFY(session.contains("m_FirstFrameNotificationGate.disarm()"));
}

void SessionDisplayTransitionTest::firstFrameGatePublishesArmAndQueueAsOneAtomicState()
{
    const QByteArray header = sourceFile(
        QStringLiteral("app/perigee/display/displaytransaction.h"));

    QVERIFY(header.contains("std::atomic<quint64> m_State"));
    QVERIFY(!header.contains("m_ArmedEvidenceEpoch"));
    QVERIFY(!header.contains("m_QueuedEvidenceEpoch"));
}

void SessionDisplayTransitionTest::sdlLoopPumpsDeadlineAndReplacementFailure()
{
    const QByteArray session = sourceFile(QStringLiteral("app/streaming/session.cpp"));
    QVERIFY(session.contains("m_TransitionCoordinator->checkDeadline()"));
    QVERIFY(session.contains("sessionConnectionFailed"));
    QVERIFY(session.contains("m_DisplayTransitionHandoff"));
    QVERIFY(session.contains("pumpFailedDisplayTransitionControl"));
    QVERIFY(session.contains("FailedTransitionControlTimeoutMs"));
    QVERIFY(session.contains("postAuthorizedTarget"));
    QVERIFY(session.contains("rollbackControlExpired"));

    const int exec = session.indexOf("void Session::exec()");
    QVERIFY(exec >= 0);

    const auto verifyPumpBeforeHandoff = [&session, exec](const QByteArray& failureReason) {
        const int failure = session.indexOf(failureReason, exec);
        QVERIFY(failure >= 0);
        const int pump = session.indexOf("pumpFailedDisplayTransitionControl();", failure);
        QVERIFY(pump > failure);
        const int handoff = session.indexOf("prepareDisplayTransitionHandoff();", failure);
        QVERIFY(handoff > pump);
    };

    verifyPumpBeforeHandoff("QStringLiteral(\"connection_failed\")");
    verifyPumpBeforeHandoff("QStringLiteral(\"window_creation_failed\")");
}

void SessionDisplayTransitionTest::dormantRecoverySessionCanDriveControlBeforeStartingStream()
{
    const QByteArray header = sourceFile(QStringLiteral("app/streaming/session.h"));
    const QByteArray session = sourceFile(QStringLiteral("app/streaming/session.cpp"));

    QVERIFY(header.contains("pumpDisplayTransitionControl"));
    QVERIFY(header.contains("displayTransitionInitializationFailed"));
    QVERIFY(header.contains("disposeDormantDisplayTransitionCarrier"));
    QVERIFY(header.contains("m_ConnectionStartRequested"));
    QVERIFY(header.contains("m_VideoSubsystemInitialized"));
    QVERIFY(session.contains("!m_Session->m_ConnectionStartRequested"));
    QVERIFY(session.contains("m_TransitionCoordinator->sessionFinished"));
    QVERIFY(session.contains("m_TransitionCoordinator->replacementPending()"));
    QVERIFY(session.contains("m_VideoSubsystemInitialized.exchange"));
    QVERIFY(session.contains("releaseVideoSubsystem"));
}

void SessionDisplayTransitionTest::recoveryControlDoesNotDependOnDeckRendererAvailability()
{
    const QByteArray session = sourceFile(QStringLiteral("app/streaming/session.cpp"));

    QVERIFY(session.contains("const bool deckAvailable ="));
    QVERIFY(session.contains("if (deckAvailable)"));
    const int probe = session.indexOf("const bool deckAvailable =");
    const int installAdapter = session.indexOf(
        "m_PolarisAdapter = std::move(polarisAdapter);", probe);
    const int optionalRenderer = session.indexOf("if (deckAvailable)", probe);
    const int completionPump = session.indexOf(
        "const bool discoveryAdvanced =", optionalRenderer);
    const int rendererGuard = session.indexOf(
        "m_DeckSurfaceRenderer == nullptr", completionPump);
    QVERIFY(probe >= 0);
    QVERIFY(installAdapter > probe);
    QVERIFY(optionalRenderer > installAdapter);
    QVERIFY(completionPump > optionalRenderer);
    QVERIFY(rendererGuard > completionPump);
}

void SessionDisplayTransitionTest::deckNavigationUpdatesAreSessionEpochScopedAndRestoreUsesNonActivatingFocus()
{
    const QByteArray session = sourceFile(QStringLiteral("app/streaming/session.cpp"));

    QVERIFY(session.contains("setDeckNavigationState(\n"
                             "            m_DisplaySessionEpoch, navigation)"));
    QVERIFY(session.contains("focusActionWithoutActivation(restoredActionId)"));
}

void SessionDisplayTransitionTest::openingControllerSelectsLayoutBeforeFirstDeckFrame()
{
    const QByteArray session = sourceFile(QStringLiteral("app/streaming/session.cpp"));
    const QByteArray delivery = sourceFile(
        QStringLiteral("app/perigee/input/deckinputdelivery.cpp"));
    const QByteArray router = sourceFile(
        QStringLiteral("app/perigee/input/deckinputrouter.cpp"));

    const int apply = session.indexOf("void Session::applyDeckInputResult");
    const int open = session.indexOf("DeckInputDelivery::openDeck(", apply);
    const int firstFrameDirty = session.indexOf(
        "m_DeckSurfaceRenderer->markDirty();", apply);
    QVERIFY(apply >= 0);
    QVERIFY(open > apply);
    QVERIFY(firstFrameDirty > open);

    const int configure = delivery.indexOf(
        "controller.setControllerLayout(result.controllerFamily");
    const int publishOpen = delivery.indexOf(
        "controller.openFromController();", configure);
    QVERIFY(configure >= 0);
    QVERIFY(publishOpen > configure);
    QVERIFY(router.contains(
        "result.controllerId = event.which;\n"
        "            result.controllerFamily =\n"
        "                ControllerLayout::familyForController(event.which);"));
}

REGISTER_PERIGEE_TEST(SessionDisplayTransitionTest);

#include "test_sessiondisplaytransition.moc"
