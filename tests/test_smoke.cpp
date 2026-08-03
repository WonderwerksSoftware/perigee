#include "test_registry.h"

#include <QtTest>

class PerigeeSmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void qtRuntimeMeetsMinimum();
};

void PerigeeSmokeTest::qtRuntimeMeetsMinimum()
{
    QVERIFY(QT_VERSION >= QT_VERSION_CHECK(6, 7, 0));
}

REGISTER_PERIGEE_TEST(PerigeeSmokeTest);

#include "test_smoke.moc"
