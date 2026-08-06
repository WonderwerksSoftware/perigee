#include "test_registry.h"

#include <QFile>
#include <QFileInfo>
#include <QtTest>

class StreamSegueQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void usesInheritedSingleSessionCleanupPath();
};

void StreamSegueQmlTest::usesInheritedSingleSessionCleanupPath()
{
    const QDir tests = QFileInfo(QString::fromUtf8(__FILE__)).dir();
    QFile file(tests.filePath(QStringLiteral("../app/gui/StreamSegue.qml")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray qml = file.readAll();

    QVERIFY(!qml.contains("DisplayTransitionCoordinator"));
    QVERIFY(!qml.contains("displayRecoverySurface"));
    QVERIFY(!qml.contains("createDisplayTransitionReplacement"));
    QVERIFY(qml.contains("function sessionReadyForDeletion()"));
}

REGISTER_PERIGEE_TEST(StreamSegueQmlTest);

#include "test_streamsegueqml.moc"
