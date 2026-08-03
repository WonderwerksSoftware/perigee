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
    void normalizesModesAndOutputsWithStableCrossKindIdentity();
    void parsesCurrentUpstreamClientSettingsEnvelope();
    void conflictingCurrentTargetsFailClosed();
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
             PolarisOperation::StopSession,
             PolarisOperation::DisplaySwitch}) {
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

void PolarisModelsTest::normalizesModesAndOutputsWithStableCrossKindIdentity()
{
    const PolarisClientSettings model = PolarisModels::parseClientSettings(
        fixture("client-settings-current.json"));

    QVERIFY(model.valid);
    QCOMPARE(model.fields.value(QStringLiteral("output_name")).desired.toString(),
             QStringLiteral("DP-1"));
    QCOMPARE(model.targets.size(), 6);
    QCOMPARE(model.targets.at(0).stableKey(),
             QStringLiteral("stream-mode:desktop_display"));
    QVERIFY(model.targets.at(0).current);
    QVERIFY(model.targets.at(1).requiresReconnect);
    QCOMPARE(model.targets.at(3).stableKey(), QStringLiteral("output:DP-1"));
    QVERIFY(model.targets.at(3).current);
    QCOMPARE(model.targets.at(4).label, QStringLiteral("Primary monitor"));
    QVERIFY(model.targets.at(3).stableKey() != model.targets.at(4).stableKey());
    QVERIFY(!model.targets.at(5).available);
    QCOMPARE(model.targets.at(5).unavailableReason,
             QStringLiteral("Display is disconnected"));
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
    QCOMPARE(model.targets.size(), 2);
    QCOMPARE(model.targets.at(0).stableKey(),
             QStringLiteral("stream-mode:desktop_display"));
    QVERIFY(model.targets.at(0).current);
    QCOMPARE(model.targets.at(1).stableKey(),
             QStringLiteral("stream-mode:headless_stream"));
}

void PolarisModelsTest::conflictingCurrentTargetsFailClosed()
{
    QJsonObject object = fixture("client-settings-current.json").json.object();
    QJsonObject capabilities = object.value(QStringLiteral("capabilities")).toObject();
    QJsonArray outputs = capabilities.value(QStringLiteral("outputs")).toArray();
    QJsonObject second = outputs.at(1).toObject();
    second.insert(QStringLiteral("active"), true);
    outputs.replace(1, second);
    capabilities.insert(QStringLiteral("outputs"), outputs);
    object.insert(QStringLiteral("capabilities"), capabilities);

    const PolarisClientSettings model = PolarisModels::parseClientSettings(
        jsonResponse(object));

    QVERIFY(model.currentConflict);
    for (const DisplayTarget& target : model.targets) {
        if (target.kind == QStringLiteral("output")) {
            QVERIFY(!target.current);
        }
    }
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

    QJsonObject settings = fixture("client-settings-current.json").json.object();
    QJsonObject settingsCapabilities = settings.value(
        QStringLiteral("capabilities")).toObject();
    QJsonArray modes = settingsCapabilities.value(QStringLiteral("modes")).toArray();
    modes.append(QJsonObject{
        {QStringLiteral("value"), QStringLiteral("DP-1")},
        {QStringLiteral("label"), QStringLiteral("Mode with output ID")},
        {QStringLiteral("available"), true},
        {QStringLiteral("restart_required"), true}});
    settingsCapabilities.insert(QStringLiteral("modes"), modes);
    settings.insert(QStringLiteral("capabilities"), settingsCapabilities);
    const PolarisClientSettings parsedSettings =
        PolarisModels::parseClientSettings(jsonResponse(settings));
    const auto modeIt = std::find_if(
        parsedSettings.targets.cbegin(), parsedSettings.targets.cend(),
        [](const DisplayTarget& target) {
            return target.stableKey() == QStringLiteral("stream-mode:DP-1");
        });
    const auto outputIt = std::find_if(
        parsedSettings.targets.cbegin(), parsedSettings.targets.cend(),
        [](const DisplayTarget& target) {
            return target.stableKey() == QStringLiteral("output:DP-1");
        });
    QVERIFY(modeIt != parsedSettings.targets.cend());
    QVERIFY(outputIt != parsedSettings.targets.cend());
    QVERIFY(modeIt->stableKey() != outputIt->stableKey());
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

    snapshot.capabilities.features.remove(QStringLiteral("display_targets_v1"));
    snapshot.session.controls.displaySelectionAllowed = false;
    snapshot.session.ownsSession = false;
    snapshot.session.transitioning = true;
    snapshot.settings.targets.clear();
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("This Polaris version does not advertise this feature"));

    snapshot.capabilities.features.insert(QStringLiteral("display_targets_v1"));
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("This paired client lacks permission"));
    snapshot.session.controls.displaySelectionAllowed = true;
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("Only the controlling client can do this"));
    snapshot.session.ownsSession = true;
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("The session is transitioning"));
    snapshot.session.transitioning = false;
    QCOMPARE(PolarisModels::availability(snapshot, PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("No alternate display is available"));
}

REGISTER_PERIGEE_TEST(PolarisModelsTest);

#include "test_polarismodels.moc"
