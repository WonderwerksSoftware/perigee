#include "perigee/polaris/polarismodels.h"
#include "perigee/polaris/polarisapiclient.h"
#include "test_registry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <algorithm>
#include <limits>

namespace {

PolarisResponse fixture(const char* name, int status = 200,
                        bool authenticated = true)
{
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
        QStringLiteral("fixtures/polaris/%1").arg(QString::fromLatin1(name)));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Unable to open Polaris fixture: %s", qPrintable(path));
    }
    PolarisResponse response;
    response.httpStatus = status;
    response.body = file.readAll();
    response.json = QJsonDocument::fromJson(response.body);
    response.authenticated = authenticated;
    return response;
}

PolarisResponse jsonResponse(QJsonObject object)
{
    PolarisResponse response;
    response.httpStatus = 200;
    response.authenticated = true;
    response.json = QJsonDocument(object);
    response.body = response.json.toJson(QJsonDocument::Compact);
    return response;
}

QUrl origin()
{
    return QUrl(QStringLiteral("https://127.0.0.1:47984"));
}

}

class PolarisModelsTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesCurrentCapabilitiesCompositionally();
    void oldCapabilitiesDisableOnlyMissingFeatures();
    void rejectsUnsafeAdvertisedEndpoints_data();
    void rejectsUnsafeAdvertisedEndpoints();
    void appliesClipboardLimitRules_data();
    void appliesClipboardLimitRules();
    void strictIntegerGuardsExclusiveUpperBoundBeforeCast();
    void exactTask13ClipboardContractUsesSessionDirectionPermissions();
    void parsesOwnerViewerTransitionAndUnknownRoles();
    void malformedStatusFailsDependentFieldsOnly();
    void tokenDependentMutationsFailClosedWithoutDisablingClipboard_data();
    void tokenDependentMutationsFailClosedWithoutDisablingClipboard();
    void parsesOfficialClientSettingsFields();
    void parsesOfficialQualityStateStrictly();
    void qualityAvailabilityRequiresContractOwnerAndTuningPermission();
    void parsesCurrentUpstreamClientSettingsEnvelope();
    void responseTrustBoundaryDistinguishesFallbackAndFailures();
    void malformedEntriesDoNotEraseIndependentValidData();
    void disabledReasonPrecedenceIsDeterministic();
};

void PolarisModelsTest::parsesCurrentCapabilitiesCompositionally()
{
    const PolarisCapabilities model = PolarisModels::parseCapabilities(
        fixture("capabilities-current.json"), origin());

    QVERIFY(model.valid);
    QVERIFY(model.isPolarisContract);
    QVERIFY(model.features.contains(QStringLiteral("named_commands_v1")));
    QVERIFY(!model.features.contains(QStringLiteral("display_targets_v1")));
    QCOMPARE(model.clientSettingsEndpoint.url.path(),
             QStringLiteral("/polaris/v1/client-settings"));
    QCOMPARE(model.commandsEndpoint.url.path(),
             QStringLiteral("/polaris/v1/commands"));
    QCOMPARE(model.sessionStatusEndpoint.url.path(),
             QStringLiteral("/polaris/v1/session/status"));
    QCOMPARE(model.maxClipboardTextBytes, qint64(262144));
    QVERIFY(model.clipboardLimitValid);
    QVERIFY(model.commands.isEmpty());
}

void PolarisModelsTest::oldCapabilitiesDisableOnlyMissingFeatures()
{
    const PolarisCapabilities model = PolarisModels::parseCapabilities(
        fixture("capabilities-old.json"), origin());

    QVERIFY(model.valid);
    QVERIFY(model.clientSettingsEndpoint.usable);
    QVERIFY(!model.commandsEndpoint.usable);
    QVERIFY(!model.features.contains(QStringLiteral("named_commands_v1")));
    QCOMPARE(model.maxClipboardTextBytes,
             PolarisModels::AbsoluteClipboardTextCeiling);
    QVERIFY(model.clipboardLimitValid);
}

void PolarisModelsTest::rejectsUnsafeAdvertisedEndpoints_data()
{
    QTest::addColumn<QString>("endpoint");
    QTest::addColumn<bool>("usable");

    QTest::newRow("canonical") << QStringLiteral("/polaris/v1/commands") << true;
    QTest::newRow("query") << QStringLiteral("/polaris/v1/commands?view=short") << true;
    QTest::newRow("absolute") << QStringLiteral("https://127.0.0.1:47984/polaris/v1/commands") << false;
    QTest::newRow("cross-origin-canary") << QStringLiteral("https://DO_NOT_RETAIN_CROSS_ORIGIN.invalid/polaris/v1/commands") << false;
    QTest::newRow("encoded-canary") << QStringLiteral("/polaris/v1/%63ommands/DO_NOT_RETAIN_ENCODED") << false;
    QTest::newRow("traversal-canary") << QStringLiteral("/polaris/v1/../DO_NOT_RETAIN_TRAVERSAL") << false;
    QTest::newRow("query-secret-canary") << QStringLiteral("/polaris/v1/commands?token=DO_NOT_RETAIN_QUERY_SECRET%ZZ") << false;
    QTest::newRow("duplicate-separator") << QStringLiteral("/polaris/v1//commands") << false;
}

void PolarisModelsTest::rejectsUnsafeAdvertisedEndpoints()
{
    QFETCH(QString, endpoint);
    QFETCH(bool, usable);
    QJsonObject object = fixture("capabilities-current.json").json.object();
    QJsonObject commands = object.value(QStringLiteral("named_commands")).toObject();
    commands.insert(QStringLiteral("endpoint"), endpoint);
    object.insert(QStringLiteral("named_commands"), commands);

    const PolarisCapabilities model = PolarisModels::parseCapabilities(
        jsonResponse(object), origin());

    QCOMPARE(model.commandsEndpoint.usable, usable);
    QVERIFY(model.clientSettingsEndpoint.usable);
    QCOMPARE(model.clipboardLimitValid, true);
    if (usable) {
        QCOMPARE(model.commandsEndpoint.advertised, endpoint);
        QVERIFY(!model.commandsEndpoint.url.isEmpty());
    }
    else {
        QVERIFY(model.commandsEndpoint.advertised.isEmpty());
        QVERIFY(model.commandsEndpoint.url.isEmpty());
        QVERIFY(!model.commandsEndpoint.errorCode.contains(
            QStringLiteral("DO_NOT_RETAIN")));
        const QString published = model.commandsEndpoint.advertised +
            model.commandsEndpoint.url.toString(QUrl::FullyEncoded) +
            model.commandsEndpoint.errorCode;
        QVERIFY2(!published.contains(QStringLiteral("DO_NOT_RETAIN")),
                 qPrintable(published));
    }
}

void PolarisModelsTest::appliesClipboardLimitRules_data()
{
    QTest::addColumn<QVariant>("value");
    QTest::addColumn<bool>("present");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<qint64>("expected");

    QTest::newRow("absent") << QVariant() << false << true
                            << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("zero") << QVariant(0) << true << true << qint64(0);
    QTest::newRow("lower") << QVariant(4096) << true << true << qint64(4096);
    QTest::newRow("over-ceiling") << QVariant(2 * 1024 * 1024) << true << true
                                  << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("negative") << QVariant(-1) << true << false
                              << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("wrong-type") << QVariant(QStringLiteral("4096")) << true << false
                                << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("exclusive-qint64-upper")
        << QVariant(9223372036854775808.0) << true << false
        << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("near-qint64-upper")
        << QVariant(9223372036854773760.0) << true << true
        << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("qint64-lower")
        << QVariant(-9223372036854775808.0) << true << false
        << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("near-qint64-lower")
        << QVariant(-9223372036854773760.0) << true << false
        << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("huge-finite") << QVariant(1.0e300) << true << false
                                  << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("positive-infinity")
        << QVariant(std::numeric_limits<double>::infinity()) << true << false
        << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("negative-infinity")
        << QVariant(-std::numeric_limits<double>::infinity()) << true << false
        << PolarisModels::AbsoluteClipboardTextCeiling;
    QTest::newRow("nan")
        << QVariant(std::numeric_limits<double>::quiet_NaN()) << true << false
        << PolarisModels::AbsoluteClipboardTextCeiling;
}

void PolarisModelsTest::appliesClipboardLimitRules()
{
    QFETCH(QVariant, value);
    QFETCH(bool, present);
    QFETCH(bool, valid);
    QFETCH(qint64, expected);
    QJsonObject object = fixture("capabilities-current.json").json.object();
    QJsonObject clipboard = object.value(QStringLiteral("clipboard")).toObject();
    if (present) {
        clipboard.insert(QStringLiteral("max_text_bytes"), QJsonValue::fromVariant(value));
    }
    else {
        clipboard.remove(QStringLiteral("max_text_bytes"));
    }
    object.insert(QStringLiteral("clipboard"), clipboard);

    const PolarisCapabilities model = PolarisModels::parseCapabilities(
        jsonResponse(object), origin());

    QCOMPARE(model.clipboardLimitValid, valid);
    QCOMPARE(model.maxClipboardTextBytes, expected);
}

void PolarisModelsTest::strictIntegerGuardsExclusiveUpperBoundBeforeCast()
{
    const QString sourcePath = QFileInfo(QString::fromUtf8(__FILE__)).dir()
        .filePath(QStringLiteral(
            "../app/perigee/polaris/polarismodels.cpp"));
    QFile source(sourcePath);
    QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(sourcePath));
    const QString implementation = QString::fromUtf8(source.readAll());
    const qsizetype upperGuard = implementation.indexOf(
        QStringLiteral("number >= ExclusiveQint64Upper"));
    const qsizetype cast = implementation.indexOf(
        QStringLiteral("*result = static_cast<qint64>(number)"));

    QVERIFY2(upperGuard >= 0,
             "strictInteger must reject 2^63 before conversion");
    QVERIFY2(cast >= 0 && upperGuard < cast,
             "the exclusive upper-bound guard must precede the cast");
}

void PolarisModelsTest::exactTask13ClipboardContractUsesSessionDirectionPermissions()
{
    PolarisDiscoverySnapshot snapshot;
    snapshot.complete = true;
    snapshot.capabilities = PolarisModels::parseCapabilities(
        fixture("capabilities-current.json"), origin());
    snapshot.capabilities.commandCatalogValid = true;
    snapshot.session = PolarisModels::parseSessionStatus(
        fixture("status-owner.json"), origin());
    snapshot.settings = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));

    QVERIFY(PolarisModels::availability(
        snapshot, PolarisOperation::ClipboardRead).enabled);
    QVERIFY(PolarisModels::availability(
        snapshot, PolarisOperation::ClipboardWrite).enabled);

    snapshot.session.controls.clipboardWriteAllowed = false;
    QCOMPARE(PolarisModels::availability(
        snapshot, PolarisOperation::ClipboardWrite).errorCode,
        QStringLiteral("permission_denied"));
    snapshot.capabilities.features.remove(QStringLiteral("clipboard_limits_v1"));
    QCOMPARE(PolarisModels::availability(
        snapshot, PolarisOperation::ClipboardRead).errorCode,
        QStringLiteral("capability_not_advertised"));
    snapshot.capabilities.features.insert(QStringLiteral("clipboard_limits_v1"));
    snapshot.capabilities.clipboardLimitValid = false;
    QCOMPARE(PolarisModels::availability(
        snapshot, PolarisOperation::ClipboardRead).errorCode,
        QStringLiteral("capability_not_advertised"));
}

void PolarisModelsTest::parsesOwnerViewerTransitionAndUnknownRoles()
{
    const PolarisSessionStatus owner = PolarisModels::parseSessionStatus(
        fixture("status-owner.json"), origin());
    QVERIFY(owner.valid);
    QVERIFY(owner.tokenValid);
    QVERIFY(owner.controllingClient);
    QVERIFY(owner.ownsSession);
    QVERIFY(owner.controls.stopAllowed);
    QVERIFY(owner.controls.stopEndpoint.usable);

    const PolarisSessionStatus viewer = PolarisModels::parseSessionStatus(
        fixture("status-viewer.json"), origin());
    QVERIFY(viewer.valid);
    QVERIFY(!viewer.controllingClient);
    QVERIFY(!viewer.ownsSession);
    QVERIFY(!viewer.controls.clipboardWriteAllowed);

    QJsonObject transition = fixture("status-owner.json").json.object();
    transition.insert(QStringLiteral("state"), QStringLiteral("stopping"));
    QVERIFY(PolarisModels::parseSessionStatus(
        jsonResponse(transition), origin()).transitioning);

    QJsonObject unknown = fixture("status-owner.json").json.object();
    unknown.insert(QStringLiteral("client_role"), QStringLiteral("future-observer"));
    const PolarisSessionStatus unknownRole = PolarisModels::parseSessionStatus(
        jsonResponse(unknown), origin());
    QVERIFY(unknownRole.valid);
    QCOMPARE(unknownRole.role, QStringLiteral("future-observer"));
    QVERIFY(!unknownRole.controllingClient);
}

void PolarisModelsTest::malformedStatusFailsDependentFieldsOnly()
{
    const PolarisSessionStatus model = PolarisModels::parseSessionStatus(
        fixture("status-malformed.json"), origin());

    QVERIFY(model.valid);
    QVERIFY(!model.tokenValid);
    QVERIFY(!model.controllingClient);
    QVERIFY(!model.ownsSession);
    QVERIFY(!model.controls.stopAllowed);
    QVERIFY(!model.controls.stopEndpoint.usable);
}

void PolarisModelsTest::tokenDependentMutationsFailClosedWithoutDisablingClipboard_data()
{
    QTest::addColumn<QVariant>("token");
    QTest::addColumn<bool>("present");

    QTest::newRow("missing") << QVariant() << false;
    QTest::newRow("wrong-type") << QVariant(22) << true;
    QTest::newRow("empty") << QVariant(QString()) << true;
}

void PolarisModelsTest::tokenDependentMutationsFailClosedWithoutDisablingClipboard()
{
    QFETCH(QVariant, token);
    QFETCH(bool, present);
    QJsonObject status = fixture("status-owner.json").json.object();
    if (present) {
        status.insert(QStringLiteral("session_token"),
                      QJsonValue::fromVariant(token));
    }
    else {
        status.remove(QStringLiteral("session_token"));
    }

    PolarisDiscoverySnapshot snapshot;
    snapshot.complete = true;
    snapshot.capabilities = PolarisModels::parseCapabilities(
        fixture("capabilities-current.json"), origin());
    snapshot.capabilities.commandCatalogValid = true;
    snapshot.session = PolarisModels::parseSessionStatus(
        jsonResponse(status), origin());
    snapshot.settings = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));

    for (const PolarisOperation operation : {
             PolarisOperation::NamedCommand,
             PolarisOperation::StopSession}) {
        const PolarisAvailability result = PolarisModels::availability(
            snapshot, operation);
        QVERIFY(!result.enabled);
        QCOMPARE(result.errorCode,
                 QStringLiteral("session_token_unavailable"));
        QCOMPARE(result.reason,
                 QStringLiteral("Current session token is unavailable"));
    }
    QVERIFY(PolarisModels::availability(
        snapshot, PolarisOperation::ClipboardRead).enabled);
}

void PolarisModelsTest::parsesOfficialClientSettingsFields()
{
    const PolarisClientSettings model = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));

    QVERIFY(model.valid);
    QCOMPARE(model.fields.size(), 4);
    const ClientSettingField field = model.fields.value(
        QStringLiteral("stream_display_mode"));
    QCOMPARE(field.direction, QStringLiteral("read_write"));
    QCOMPARE(field.scope, QStringLiteral("host"));
    QCOMPARE(field.effective.toString(), QStringLiteral("desktop_display"));
    QVERIFY(!field.requiresReconnect);
}

void PolarisModelsTest::parsesOfficialQualityStateStrictly()
{
    const PolarisClientSettings model = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));

    QVERIFY(model.valid);
    QCOMPARE(model.adaptiveBitrateEnabled, std::optional<bool>(true));
    QCOMPARE(model.aiAutoQualityEnabled, std::optional<bool>(true));
    QCOMPARE(model.aiOptimizerEnabled, std::optional<bool>(true));
    QCOMPARE(model.adaptiveTargetBitrateKbps, std::optional<int>(28000));
    QCOMPARE(model.adaptiveBaseBitrateKbps, std::optional<int>(35000));
    QVERIFY(!model.adaptiveMinBitrateKbps.has_value());
    QVERIFY(!model.adaptiveMaxBitrateKbps.has_value());
    QVERIFY(!model.encoderBitrateKbps.has_value());

    const PolarisSessionStatus session = PolarisModels::parseSessionStatus(
        fixture("status-owner.json"), origin());
    QCOMPARE(session.adaptiveBitrateEnabled, std::optional<bool>(true));
    QCOMPARE(session.adaptiveTargetBitrateKbps, std::optional<int>(28000));
    QCOMPARE(session.adaptiveBaseBitrateKbps, std::optional<int>(35000));
    QCOMPARE(session.adaptiveMinBitrateKbps, std::optional<int>(2000));
    QCOMPARE(session.adaptiveMaxBitrateKbps, std::optional<int>(100000));
    QCOMPARE(session.encoderBitrateKbps, std::optional<int>(27500));
    QCOMPARE(session.adaptiveState, QStringLiteral("steady"));
    QCOMPARE(session.adaptiveReason, QStringLiteral("Network is stable"));

    QJsonObject malformed = fixture("status-owner.json").json.object();
    QJsonObject tuning = malformed.value(QStringLiteral("tuning")).toObject();
    tuning.insert(QStringLiteral("adaptive_base_bitrate_kbps"), 35000.5);
    tuning.insert(QStringLiteral("adaptive_min_bitrate_kbps"), -1);
    malformed.insert(QStringLiteral("tuning"), tuning);
    QJsonObject encoder = malformed.value(QStringLiteral("encoder")).toObject();
    encoder.insert(QStringLiteral("bitrate_kbps"), QStringLiteral("27500"));
    malformed.insert(QStringLiteral("encoder"), encoder);

    const PolarisSessionStatus strict = PolarisModels::parseSessionStatus(
        jsonResponse(malformed), origin());
    QVERIFY(!strict.adaptiveBaseBitrateKbps.has_value());
    QVERIFY(!strict.adaptiveMinBitrateKbps.has_value());
    QVERIFY(!strict.encoderBitrateKbps.has_value());
    QCOMPARE(strict.adaptiveTargetBitrateKbps, std::optional<int>(28000));
}

void PolarisModelsTest::qualityAvailabilityRequiresContractOwnerAndTuningPermission()
{
    PolarisDiscoverySnapshot snapshot;
    snapshot.complete = true;
    snapshot.capabilities = PolarisModels::parseCapabilities(
        fixture("capabilities-current.json"), origin());
    snapshot.session = PolarisModels::parseSessionStatus(
        fixture("status-owner.json"), origin());
    snapshot.settings = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));

    QVERIFY(PolarisModels::availability(
        snapshot, PolarisOperation::BitrateControl).enabled);
    QVERIFY(PolarisModels::availability(
        snapshot, PolarisOperation::AdaptiveQualityControl).enabled);

    snapshot.session.controls.hostTuningAllowed = false;
    QCOMPARE(PolarisModels::availability(
                 snapshot, PolarisOperation::BitrateControl).errorCode,
             QStringLiteral("permission_denied"));
    snapshot.session.controls.hostTuningAllowed = true;
    snapshot.session.ownsSession = false;
    QCOMPARE(PolarisModels::availability(
                 snapshot, PolarisOperation::AdaptiveQualityControl).errorCode,
             QStringLiteral("not_controlling_client"));
    snapshot.session.ownsSession = true;
    snapshot.capabilities.features.remove(
        QStringLiteral("ai_auto_quality_control"));
    QCOMPARE(PolarisModels::availability(
                 snapshot, PolarisOperation::AdaptiveQualityControl).errorCode,
             QStringLiteral("capability_not_advertised"));
}

void PolarisModelsTest::parsesCurrentUpstreamClientSettingsEnvelope()
{
    const PolarisClientSettings model = PolarisModels::parseClientSettings(
        fixture("client-settings-upstream-envelope.json"));

    QVERIFY(model.valid);
    QVERIFY(model.fields.contains(QStringLiteral("stream_display_mode")));
    QVERIFY(!model.fields.contains(QStringLiteral("unknown_duplicate")));
    QCOMPARE(model.fields.value(QStringLiteral("stream_display_mode"))
                 .effective.toString(),
             QStringLiteral("desktop_display"));
}

void PolarisModelsTest::responseTrustBoundaryDistinguishesFallbackAndFailures()
{
    PolarisResponse notFound;
    notFound.httpStatus = 404;
    notFound.authenticated = true;
    QVERIFY(PolarisModels::parseCapabilities(notFound, origin()).standardHost);

    PolarisResponse notPolaris = jsonResponse({{QStringLiteral("server"),
                                                QStringLiteral("sunshine")}});
    QVERIFY(PolarisModels::parseCapabilities(notPolaris, origin()).standardHost);

    PolarisResponse network;
    network.errorCode = QStringLiteral("network_error");
    const PolarisCapabilities unreachable = PolarisModels::parseCapabilities(
        network, origin());
    QCOMPARE(unreachable.errorCode, QStringLiteral("polaris_unreachable"));

    PolarisResponse tls;
    tls.errorCode = QStringLiteral("tls_identity_mismatch");
    const PolarisCapabilities identityFailure = PolarisModels::parseCapabilities(
        tls, origin());
    QCOMPARE(identityFailure.errorCode, QStringLiteral("tls_identity_mismatch"));

    PolarisResponse permission;
    permission.httpStatus = 403;
    permission.authenticated = true;
    permission.errorCode = QStringLiteral("http_error");
    QCOMPARE(PolarisModels::parseSessionStatus(permission, origin()).errorCode,
             QStringLiteral("permission_denied"));

    PolarisResponse unauthorized = permission;
    unauthorized.httpStatus = 401;
    QCOMPARE(PolarisModels::parseSessionStatus(unauthorized, origin()).errorCode,
             QStringLiteral("authentication_failed"));

    PolarisResponse spoofedPermission = permission;
    spoofedPermission.authenticated = false;
    QCOMPARE(PolarisModels::parseSessionStatus(
        spoofedPermission, origin()).errorCode,
        QStringLiteral("authentication_failed"));
}

void PolarisModelsTest::malformedEntriesDoNotEraseIndependentValidData()
{
    const PolarisCapabilities parsedCapabilities =
        PolarisModels::parseCapabilities(
            fixture("capabilities-current.json"), origin());
    QVERIFY(parsedCapabilities.commands.isEmpty());
    QVERIFY(parsedCapabilities.clientSettingsEndpoint.usable);
    QCOMPARE(parsedCapabilities.maxClipboardTextBytes, qint64(262144));

    const PolarisClientSettings parsedSettings =
        PolarisModels::parseClientSettings(
            fixture("client-settings-current.json"));
    QVERIFY(parsedSettings.valid);
    QVERIFY(parsedSettings.fields.contains(
        QStringLiteral("stream_display_mode")));
}

void PolarisModelsTest::disabledReasonPrecedenceIsDeterministic()
{
    PolarisDiscoverySnapshot snapshot;
    snapshot.capabilities = PolarisModels::parseCapabilities(
        fixture("capabilities-current.json"), origin());
    snapshot.session = PolarisModels::parseSessionStatus(
        fixture("status-owner.json"), origin());
    snapshot.settings = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));
    snapshot.complete = true;

    snapshot.capabilities.features.remove(QStringLiteral("session_stop_v1"));
    snapshot.session.controls.stopAllowed = false;
    snapshot.session.ownsSession = false;
    snapshot.session.transitioning = true;
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::StopSession).reason,
             QStringLiteral("This Polaris version does not advertise this feature"));

    snapshot.capabilities.features.insert(QStringLiteral("session_stop_v1"));
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::StopSession).reason,
             QStringLiteral("This paired client lacks permission"));
    snapshot.session.controls.stopAllowed = true;
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::StopSession).reason,
             QStringLiteral("Only the controlling client can do this"));
    snapshot.session.ownsSession = true;
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::StopSession).reason,
             QStringLiteral("The session is transitioning"));
    snapshot.session.transitioning = false;
    QVERIFY(PolarisModels::availability(
        snapshot, PolarisOperation::StopSession).enabled);
}

REGISTER_PERIGEE_TEST(PolarisModelsTest);

#include "test_polarismodels.moc"
