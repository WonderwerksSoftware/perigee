#include "perigee/support/redaction.h"
#include "test_registry.h"

#include <QtTest>

class RedactionTest : public QObject
{
    Q_OBJECT

private slots:
    void redactsAllQueryValuesAndDropsFragments();
    void redactsSensitivePathValuesAndRejectsBodyLikeInput();
    void redactsSessionValuesAndSensitiveHeaderLikeInput();
    void failsClosedForEncodedPathAndHeaderMaterial();
    void sanitizesUnsafeQueryNamesAndRedactsEncodedValues();
    void removesControlCharactersAndLogInjection();
};

void RedactionTest::redactsAllQueryValuesAndDropsFragments()
{
    const QString redacted = PerigeeRedaction::pathForLog(
        QStringLiteral("/polaris/v1/status?token=CANARY_TOKEN&plain=CANARY_VALUE#CANARY_FRAGMENT"));

    QCOMPARE(redacted,
             QStringLiteral("/polaris/v1/status?token=<redacted>&plain=<redacted>"));
    QVERIFY(!redacted.contains(QStringLiteral("CANARY")));
}

void RedactionTest::redactsSensitivePathValuesAndRejectsBodyLikeInput()
{
    const QString path = PerigeeRedaction::pathForLog(
        QStringLiteral("/polaris/v1/command/CANARY_COMMAND/key/CANARY_KEY"));
    QCOMPARE(path,
             QStringLiteral("/polaris/v1/command/<redacted>/key/<redacted>"));

    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("{\"clipboard\":\"CANARY_BODY\"}")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("Authorization: Bearer CANARY_HEADER")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("/polaris/v1/commands/CANARY_COMMAND/clipboard/CANARY_CLIPBOARD")),
             QStringLiteral("/polaris/v1/commands/<redacted>/clipboard/<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("-----BEGIN PRIVATE KEY-----\nCANARY_PRIVATE_KEY")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("X-Client-Certificate: CANARY_CLIENT_CERT")),
             QStringLiteral("<redacted>"));
}

void RedactionTest::redactsSessionValuesAndSensitiveHeaderLikeInput()
{
    const QString path = PerigeeRedaction::pathForLog(
        QStringLiteral("/polaris/v1/sessions/CANARY_SESSION/"
                       "tokens/CANARY_TOKEN/clipboards/CANARY_CLIPBOARD"));
    QCOMPARE(path,
             QStringLiteral("/polaris/v1/sessions/<redacted>/"
                            "tokens/<redacted>/clipboards/<redacted>"));
    QVERIFY(!path.contains(QStringLiteral("CANARY")));

    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("Session: CANARY_SESSION_HEADER")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("Token: CANARY_TOKEN_HEADER")),
             QStringLiteral("<redacted>"));
}

void RedactionTest::failsClosedForEncodedPathAndHeaderMaterial()
{
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("Authorization%3A%20Bearer%20CANARY_HEADER")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("/polaris/v1/comm%61nds/CANARY_COMMAND")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("Authorization   : Bearer CANARY_HEADER")),
             QStringLiteral("<redacted>"));
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("polaris/v1/status?token=CANARY_TOKEN")),
             QStringLiteral("<redacted>"));
}

void RedactionTest::sanitizesUnsafeQueryNamesAndRedactsEncodedValues()
{
    QCOMPARE(PerigeeRedaction::pathForLog(
                 QStringLiteral("/polaris/v1/status?label=Desk%20One")),
             QStringLiteral(
                 "/polaris/v1/status?label=<redacted>"));

    const QString redacted = PerigeeRedaction::pathForLog(
        QStringLiteral("/polaris/v1/status?to%6ben=CANARY_TOKEN&"
                       "bad/name=CANARY_NAME"));
    QCOMPARE(redacted,
             QStringLiteral("/polaris/v1/status?"
                            "<redacted>=<redacted>&"
                            "<redacted>=<redacted>"));
    QVERIFY(!redacted.contains(QStringLiteral("CANARY")));
    QVERIFY(!redacted.contains(QLatin1Char('%')));
}

void RedactionTest::removesControlCharactersAndLogInjection()
{
    const QString redacted = PerigeeRedaction::pathForLog(
        QStringLiteral("/polaris/v1/status\nFORGED=CANARY_INJECTION"));

    QVERIFY(!redacted.contains(QLatin1Char('\n')));
    QVERIFY(!redacted.contains(QLatin1Char('\r')));
    QVERIFY(!redacted.contains(QStringLiteral("CANARY_INJECTION")));
}

REGISTER_PERIGEE_TEST(RedactionTest);

#include "test_redaction.moc"
