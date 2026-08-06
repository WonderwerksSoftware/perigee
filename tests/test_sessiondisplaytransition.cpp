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
        qFatal("Unable to read source contract file: %s",
               qPrintable(file.fileName()));
    }
    return file.readAll();
}

}

class SessionPhysicalDisplayTest : public QObject
{
    Q_OBJECT

private slots:
    void shortcutAndFreshFrameEvidenceUseTheSdlThread();
    void staleSessionEventsCannotCompleteAReplacementSession();
    void deadlineAndCancellationStayIndependentOfDeckRendering();
};

void SessionPhysicalDisplayTest::shortcutAndFreshFrameEvidenceUseTheSdlThread()
{
    const QByteArray codes = sourceFile(
        QStringLiteral("app/streaming/sdleventcodes.h"));
    const QByteArray session = sourceFile(
        QStringLiteral("app/streaming/session.cpp"));

    QVERIFY(codes.contains("SDL_CODE_PERIGEE_POST_SHORTCUT_FRAME"));
    QVERIFY(session.contains("sendPhysicalDisplayShortcut(displayNumber)"));
    QVERIFY(session.contains(
        "m_PhysicalDisplayController->notifyAcceptedFrame"));
    QVERIFY(session.contains(
        "m_PhysicalDisplayController->observeFreshFrame"));
    QVERIFY(session.contains("SDL_CODE_PERIGEE_POST_SHORTCUT_FRAME"));
    QVERIFY(session.contains("event.user.data1"));
    QVERIFY(session.contains("event.user.data2"));
}

void SessionPhysicalDisplayTest::staleSessionEventsCannotCompleteAReplacementSession()
{
    const QByteArray header = sourceFile(
        QStringLiteral("app/streaming/session.h"));
    const QByteArray session = sourceFile(
        QStringLiteral("app/streaming/session.cpp"));

    QVERIFY(header.contains("m_PhysicalDisplaySessionEpoch"));
    QVERIFY(header.contains("s_NextPhysicalDisplaySessionEpoch"));
    QVERIFY(session.contains(
        "sessionEpoch == m_PhysicalDisplaySessionEpoch"));
    QVERIFY(session.contains(
        "session->m_PhysicalDisplaySessionEpoch"));
}

void SessionPhysicalDisplayTest::deadlineAndCancellationStayIndependentOfDeckRendering()
{
    const QByteArray session = sourceFile(
        QStringLiteral("app/streaming/session.cpp"));

    QVERIFY(session.contains(
        "m_PhysicalDisplayController->checkDeadline()"));
    QVERIFY(session.contains("m_PhysicalDisplayController->cancel()"));

    const int pump = session.indexOf("void Session::pumpDeckUi()");
    const int deadline = session.indexOf(
        "m_PhysicalDisplayController->checkDeadline()", pump);
    const int rendererGuard = session.indexOf(
        "m_DeckController == nullptr || m_DeckSurfaceRenderer == nullptr",
        pump);
    QVERIFY(pump >= 0);
    QVERIFY(deadline > pump);
    QVERIFY(rendererGuard > deadline);
}

REGISTER_PERIGEE_TEST(SessionPhysicalDisplayTest);

#include "test_sessiondisplaytransition.moc"
