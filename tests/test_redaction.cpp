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
    void redactsCompoundSensitivePathValues_data();
    void redactsCompoundSensitivePathValues();
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

void RedactionTest::redactsCompoundSensitivePathValues_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("hyphen-session-token")
        << QStringLiteral(
               "/polaris/v1/session-token/CANARY_SESSION_TOKEN")
        << QStringLiteral("/polaris/v1/session-token/<redacted>");
    QTest::newRow("underscore-authorization-key")
        << QStringLiteral(
               "/polaris/v1/authorization_key/CANARY_AUTH_KEY")
        << QStringLiteral("/polaris/v1/authorization_key/<redacted>");
    QTest::newRow("hyphen-authorization-key")
        << QStringLiteral(
               "/polaris/v1/authorization-key/CANARY_AUTH_KEY")
        << QStringLiteral("/polaris/v1/authorization-key/<redacted>");
    QTest::newRow("case-insensitive-dot-client-certificate")
        << QStringLiteral(
               "/polaris/v1/CLIENT.CERTIFICATE/CANARY_CERT")
        << QStringLiteral("/polaris/v1/CLIENT.CERTIFICATE/<redacted>");
    QTest::newRow("hyphen-client-certificate")
        << QStringLiteral(
               "/polaris/v1/client-certificate/CANARY_CERT")
        << QStringLiteral("/polaris/v1/client-certificate/<redacted>");
    QTest::newRow("hyphen-clipboard-command")
        << QStringLiteral(
               "/polaris/v1/clipboard-command/CANARY_CLIPBOARD_COMMAND")
        << QStringLiteral("/polaris/v1/clipboard-command/<redacted>");
    QTest::newRow("encoded-compound-label-fails-closed")
        << QStringLiteral(
               "/polaris/v1/session%2Dtoken/CANARY_ENCODED_TOKEN")
        << QStringLiteral("<redacted>");
    QTest::newRow("keyboard-is-not-key")
        << QStringLiteral("/polaris/v1/keyboard-layout/colemak")
        << QStringLiteral("/polaris/v1/keyboard-layout/colemak");
    QTest::newRow("hockey-is-not-key")
        << QStringLiteral("/polaris/v1/hockey-score/3-2")
        << QStringLiteral("/polaris/v1/hockey-score/3-2");
    QTest::newRow("monkey-is-not-key")
        << QStringLiteral("/polaris/v1/monkey/capuchin")
        << QStringLiteral("/polaris/v1/monkey/capuchin");
}

void RedactionTest::redactsCompoundSensitivePathValues()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    const QString redacted = PerigeeRedaction::pathForLog(input);

    QCOMPARE(redacted, expected);
    if (input.contains(QStringLiteral("CANARY"))) {
        QVERIFY(!redacted.contains(QStringLiteral("CANARY")));
    }
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
