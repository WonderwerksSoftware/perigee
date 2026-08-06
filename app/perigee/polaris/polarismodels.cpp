#include "polarismodels.h"

#include "polarisapiclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double InclusiveQint64Lower = -0x1p63;
constexpr double ExclusiveQint64Upper = 0x1p63;

QString responseError(const PolarisResponse& response)
{
    if (response.errorCode == QStringLiteral("network_error") ||
            response.errorCode == QStringLiteral("timeout")) {
        return QStringLiteral("polaris_unreachable");
    }
    if (response.errorCode == QStringLiteral("tls_identity_mismatch")) {
        return response.errorCode;
    }
    if (!response.authenticated) {
        return QStringLiteral("authentication_failed");
    }
    if (response.httpStatus == 401) {
        return QStringLiteral("authentication_failed");
    }
    if (response.httpStatus == 403) {
        return QStringLiteral("permission_denied");
    }
    if (!response.errorCode.isEmpty()) {
        return response.errorCode;
    }
    if (response.httpStatus < 200 || response.httpStatus >= 300) {
        return QStringLiteral("http_error");
    }
    return {};
}

AdvertisedPolarisEndpoint parseEndpoint(const QUrl& origin,
                                        const QJsonValue& value)
{
    AdvertisedPolarisEndpoint endpoint;
    if (!value.isString()) {
        endpoint.errorCode = QStringLiteral("endpoint_missing");
        return endpoint;
    }
    const QString advertised = value.toString();
    endpoint.url = PolarisApiClient::resolveAdvertisedEndpoint(
        origin, advertised, &endpoint.errorCode);
    endpoint.usable = endpoint.url.isValid();
    if (endpoint.usable) {
        endpoint.advertised = advertised;
    }
    else {
        endpoint.url = QUrl();
    }
    return endpoint;
}

bool strictBool(const QJsonObject& object, const QString& name,
                bool fallback = false)
{
    const QJsonValue value = object.value(name);
    return value.isBool() ? value.toBool() : fallback;
}

bool strictInteger(const QJsonValue& value, qint64* result)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
            number < InclusiveQint64Lower ||
            number >= ExclusiveQint64Upper) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

std::optional<int> strictPositiveInt(const QJsonObject& object,
                                     const QString& name)
{
    qint64 value = 0;
    if (!strictInteger(object.value(name), &value) || value <= 0 ||
            value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<bool> strictOptionalBool(const QJsonObject& object,
                                       const QString& name)
{
    const QJsonValue value = object.value(name);
    return value.isBool() ? std::optional<bool>(value.toBool())
                          : std::nullopt;
}

bool safeCommandIdentifier(const QString& value)
{
    if (value.isEmpty() || value.size() > 128) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return (code >= 'a' && code <= 'z') ||
            (code >= 'A' && code <= 'Z') ||
            (code >= '0' && code <= '9') || code == '-' || code == '_' ||
            code == '.' || code == ':';
    });
}

bool isTransitionState(const QString& state)
{
    const QString normalized = state.trimmed().toLower();
    return normalized == QStringLiteral("transitioning") ||
        normalized == QStringLiteral("stopping") ||
        normalized == QStringLiteral("reconnecting") ||
        normalized == QStringLiteral("starting") ||
        normalized == QStringLiteral("pending_relaunch");
}

PolarisAvailability disabled(PolarisAvailabilityCode code,
                             const QString& errorCode,
                             const QString& reason)
{
    return {false, code, errorCode, reason};
}

std::optional<PolarisAvailability> dependencyFailure(
    const QString& errorCode)
{
    if (errorCode.isEmpty()) {
        return std::nullopt;
    }
    if (errorCode == QStringLiteral("polaris_unreachable")) {
        return disabled(PolarisAvailabilityCode::Unreachable, errorCode,
                        QStringLiteral("Polaris is unreachable"));
    }
    if (errorCode == QStringLiteral("tls_identity_mismatch")) {
        return disabled(PolarisAvailabilityCode::TlsIdentityMismatch,
                        errorCode,
                        QStringLiteral("Polaris identity verification failed"));
    }
    if (errorCode == QStringLiteral("authentication_failed")) {
        return disabled(PolarisAvailabilityCode::AuthenticationFailed,
                        errorCode,
                        QStringLiteral("Polaris authentication failed"));
    }
    if (errorCode == QStringLiteral("permission_denied")) {
        return disabled(PolarisAvailabilityCode::PermissionDenied,
                        errorCode,
                        QStringLiteral("This paired client lacks permission"));
    }
    return disabled(PolarisAvailabilityCode::MalformedResponse, errorCode,
                    QStringLiteral("Polaris returned invalid discovery data"));
}

bool operationCapabilityAvailable(const PolarisDiscoverySnapshot& snapshot,
                                  PolarisOperation operation)
{
    const auto& capabilities = snapshot.capabilities;
    switch (operation) {
    case PolarisOperation::ClipboardRead:
    case PolarisOperation::ClipboardWrite:
        return capabilities.features.contains(
            QStringLiteral("clipboard_limits_v1")) &&
            capabilities.clipboardLimitValid;
    case PolarisOperation::NamedCommand:
        return capabilities.features.contains(
            QStringLiteral("named_commands_v1")) &&
            capabilities.commandsEndpoint.usable;
    case PolarisOperation::StopSession:
        return capabilities.features.contains(
            QStringLiteral("session_stop_v1"));
    case PolarisOperation::BitrateControl:
        return capabilities.features.contains(
                   QStringLiteral("client_settings_v1")) &&
            capabilities.features.contains(
                   QStringLiteral("adaptive_bitrate_control"));
    case PolarisOperation::AdaptiveQualityControl:
        return capabilities.features.contains(
                   QStringLiteral("client_settings_v1")) &&
            capabilities.features.contains(
                   QStringLiteral("adaptive_bitrate_control")) &&
            capabilities.features.contains(
                   QStringLiteral("ai_auto_quality_control"));
    }
    return false;
}

bool operationPermissionGranted(const PolarisDiscoverySnapshot& snapshot,
                                PolarisOperation operation)
{
    switch (operation) {
    case PolarisOperation::ClipboardRead:
        return snapshot.session.controls.clipboardReadAllowed;
    case PolarisOperation::ClipboardWrite:
        return snapshot.session.controls.clipboardWriteAllowed;
    case PolarisOperation::NamedCommand:
        return snapshot.session.controls.commandsAllowed;
    case PolarisOperation::StopSession:
        return snapshot.session.controls.stopAllowed;
    case PolarisOperation::BitrateControl:
    case PolarisOperation::AdaptiveQualityControl:
        return snapshot.session.controls.hostTuningAllowed;
    }
    return false;
}

bool requiresControlOwnership(PolarisOperation operation)
{
    return operation == PolarisOperation::NamedCommand ||
        operation == PolarisOperation::StopSession ||
        operation == PolarisOperation::BitrateControl ||
        operation == PolarisOperation::AdaptiveQualityControl;
}

bool requiresSessionToken(PolarisOperation operation)
{
    return operation == PolarisOperation::NamedCommand ||
        operation == PolarisOperation::StopSession;
}

}

PolarisCapabilities PolarisModels::parseCapabilities(
    const PolarisResponse& response, const QUrl& pairedOrigin)
{
    PolarisCapabilities result;
    result.errorCode = responseError(response);
    if (result.errorCode == QStringLiteral("http_error") &&
            response.authenticated && response.httpStatus == 404) {
        result.errorCode.clear();
        result.standardHost = true;
        return result;
    }
    if (!result.errorCode.isEmpty()) {
        return result;
    }
    if (!response.json.isObject()) {
        result.standardHost = true;
        return result;
    }

    const QJsonObject root = response.json.object();
    if (root.value(QStringLiteral("server")) != QStringLiteral("polaris")) {
        result.standardHost = true;
        return result;
    }
    result.valid = true;
    result.isPolarisContract = true;

    const QJsonObject features = root.value(QStringLiteral("features")).toObject();
    for (auto it = features.constBegin(); it != features.constEnd(); ++it) {
        if (it.value().isBool() && it.value().toBool()) {
            result.features.insert(it.key());
        }
    }

    const QJsonObject session = root.value(QStringLiteral("session")).toObject();
    result.sessionStatusEndpoint = parseEndpoint(
        pairedOrigin, session.value(QStringLiteral("status_endpoint")));
    const QJsonObject clientSettings = root.value(
        QStringLiteral("client_settings")).toObject();
    result.clientSettingsEndpoint = parseEndpoint(
        pairedOrigin, clientSettings.value(QStringLiteral("endpoint")));
    const QJsonObject namedCommands = root.value(
        QStringLiteral("named_commands")).toObject();
    result.commandsEndpoint = parseEndpoint(
        pairedOrigin, namedCommands.value(QStringLiteral("endpoint")));

    const QJsonObject clipboard = root.value(QStringLiteral("clipboard")).toObject();
    if (clipboard.contains(QStringLiteral("max_text_bytes"))) {
        qint64 maximum = 0;
        if (!strictInteger(clipboard.value(QStringLiteral("max_text_bytes")),
                           &maximum) || maximum < 0) {
            result.clipboardLimitValid = false;
        }
        else {
            result.maxClipboardTextBytes = std::min(
                maximum, AbsoluteClipboardTextCeiling);
        }
    }

    return result;
}

PolarisCommandCatalog PolarisModels::parseCommands(
    const PolarisResponse& response,
    const AdvertisedPolarisEndpoint& validatedEndpoint)
{
    PolarisCommandCatalog result;
    result.errorCode = responseError(response);
    if (!result.errorCode.isEmpty()) {
        return result;
    }
    if (!validatedEndpoint.usable || !response.json.isObject() ||
            !response.json.object().value(
                QStringLiteral("commands")).isArray()) {
        result.errorCode = QStringLiteral("malformed_response");
        return result;
    }

    result.valid = true;
    const QJsonArray commands = response.json.object().value(
        QStringLiteral("commands")).toArray();
    QSet<int> seenIndexes;
    for (const QJsonValue& value : commands) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject command = value.toObject();
        qint64 index = -1;
        if (!strictInteger(command.value(QStringLiteral("index")), &index) ||
                index < 0 || index > std::numeric_limits<int>::max() ||
                seenIndexes.contains(static_cast<int>(index)) ||
                !command.value(QStringLiteral("name")).isString() ||
                command.value(QStringLiteral("name")).toString().trimmed().isEmpty() ||
                !command.value(QStringLiteral("risk")).isString()) {
            continue;
        }
        NamedCommandMetadata metadata;
        metadata.index = static_cast<int>(index);
        const QString advertisedId = command.value(
            QStringLiteral("id")).toString();
        metadata.identifier = safeCommandIdentifier(advertisedId)
            ? advertisedId : QString::number(index);
        metadata.displayName = command.value(QStringLiteral("name")).toString();
        metadata.risk = command.value(QStringLiteral("risk")).toString();
        metadata.endpoint = validatedEndpoint;
        result.commands.push_back(std::move(metadata));
        seenIndexes.insert(static_cast<int>(index));
    }
    return result;
}

PolarisSessionStatus PolarisModels::parseSessionStatus(
    const PolarisResponse& response, const QUrl& pairedOrigin)
{
    PolarisSessionStatus result;
    result.errorCode = responseError(response);
    if (!result.errorCode.isEmpty()) {
        return result;
    }
    if (!response.json.isObject()) {
        result.errorCode = QStringLiteral("malformed_response");
        return result;
    }
    result.valid = true;
    const QJsonObject root = response.json.object();
    if (root.value(QStringLiteral("state")).isString()) {
        result.state = root.value(QStringLiteral("state")).toString();
    }
    if (root.value(QStringLiteral("session_token")).isString()) {
        result.sessionToken = root.value(QStringLiteral("session_token")).toString();
        result.tokenValid = !result.sessionToken.isEmpty();
    }
    if (root.value(QStringLiteral("client_role")).isString()) {
        result.role = root.value(QStringLiteral("client_role")).toString();
        result.controllingClient = result.role == QStringLiteral("owner") ||
            result.role == QStringLiteral("controller");
    }
    result.ownsSession = strictBool(root, QStringLiteral("owned_by_client"));

    const QJsonObject controls = root.value(QStringLiteral("controls")).toObject();
    result.controls.stopAllowed = strictBool(
        controls, QStringLiteral("stop_allowed"));
    result.controls.hostTuningAllowed = strictBool(
        controls, QStringLiteral("host_tuning_allowed"));
    result.controls.commandsAllowed = strictBool(
        controls, QStringLiteral("quit_allowed")) &&
        strictBool(controls, QStringLiteral("client_commands_enabled")) &&
        strictBool(controls, QStringLiteral("device_commands_enabled"));
    result.controls.clipboardReadAllowed = strictBool(
        controls, QStringLiteral("clipboard_read_allowed"));
    result.controls.clipboardWriteAllowed = strictBool(
        controls, QStringLiteral("clipboard_write_allowed"));
    result.controls.stopEndpoint = parseEndpoint(
        pairedOrigin, controls.value(QStringLiteral("stop_endpoint")));
    const QJsonObject tuning = root.value(QStringLiteral("tuning")).toObject();
    const QJsonObject encoder = root.value(QStringLiteral("encoder")).toObject();
    result.adaptiveBitrateEnabled = strictOptionalBool(
        tuning, QStringLiteral("adaptive_bitrate_enabled"));
    if (!result.adaptiveBitrateEnabled.has_value()) {
        result.adaptiveBitrateEnabled = strictOptionalBool(
            root, QStringLiteral("adaptive_bitrate_enabled"));
    }
    result.aiAutoQualityEnabled = strictOptionalBool(
        tuning, QStringLiteral("ai_auto_quality_enabled"));
    if (!result.aiAutoQualityEnabled.has_value()) {
        result.aiAutoQualityEnabled = strictOptionalBool(
            root, QStringLiteral("ai_auto_quality_enabled"));
    }
    result.aiOptimizerEnabled = strictOptionalBool(
        tuning, QStringLiteral("ai_optimizer_enabled"));
    if (!result.aiOptimizerEnabled.has_value()) {
        result.aiOptimizerEnabled = strictOptionalBool(
            root, QStringLiteral("ai_optimizer_enabled"));
    }
    result.adaptiveTargetBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_target_bitrate_kbps"));
    if (!result.adaptiveTargetBitrateKbps.has_value()) {
        result.adaptiveTargetBitrateKbps = strictPositiveInt(
            root, QStringLiteral("adaptive_target_bitrate_kbps"));
    }
    result.adaptiveBaseBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_base_bitrate_kbps"));
    result.adaptiveMinBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_min_bitrate_kbps"));
    result.adaptiveMaxBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_max_bitrate_kbps"));
    result.encoderBitrateKbps = strictPositiveInt(
        encoder, QStringLiteral("bitrate_kbps"));
    const QJsonValue adaptiveState = tuning.value(
        QStringLiteral("adaptive_bitrate_state"));
    const QJsonValue rootAdaptiveState = root.value(
        QStringLiteral("adaptive_bitrate_state"));
    if (adaptiveState.isString()) {
        result.adaptiveState = adaptiveState.toString();
    }
    else if (rootAdaptiveState.isString()) {
        result.adaptiveState = rootAdaptiveState.toString();
    }
    const QJsonValue adaptiveReason = tuning.value(
        QStringLiteral("adaptive_bitrate_reason"));
    const QJsonValue rootAdaptiveReason = root.value(
        QStringLiteral("adaptive_bitrate_reason"));
    if (adaptiveReason.isString()) {
        result.adaptiveReason = adaptiveReason.toString();
    }
    else if (rootAdaptiveReason.isString()) {
        result.adaptiveReason = rootAdaptiveReason.toString();
    }
    result.transitioning = isTransitionState(result.state) ||
        strictBool(root, QStringLiteral("shutdown_requested")) ||
        strictBool(controls, QStringLiteral("shutdown_in_progress"));
    return result;
}

PolarisClientSettings PolarisModels::parseClientSettings(
    const PolarisResponse& response)
{
    PolarisClientSettings result;
    result.errorCode = responseError(response);
    if (!result.errorCode.isEmpty()) {
        return result;
    }
    if (!response.json.isObject()) {
        result.errorCode = QStringLiteral("malformed_response");
        return result;
    }
    result.valid = true;
    const QJsonObject envelope = response.json.object();
    const QJsonObject root = envelope.value(QStringLiteral("client_settings")).isObject()
        ? envelope.value(QStringLiteral("client_settings")).toObject()
        : envelope;
    QJsonObject fields = root.value(QStringLiteral("sync_status")).toObject()
        .value(QStringLiteral("fields")).toObject();
    const QJsonObject directFields = root.value(
        QStringLiteral("fields")).toObject();
    for (auto it = directFields.constBegin(); it != directFields.constEnd(); ++it) {
        fields.insert(it.key(), it.value());
    }
    if (fields.isEmpty() && root != envelope) {
        fields = envelope.value(QStringLiteral("sync_status")).toObject()
            .value(QStringLiteral("fields")).toObject();
    }
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject object = it.value().toObject();
        if (!object.value(QStringLiteral("direction")).isString() ||
                !object.value(QStringLiteral("scope")).isString()) {
            continue;
        }
        ClientSettingField field;
        field.name = it.key();
        field.direction = object.value(QStringLiteral("direction")).toString();
        field.scope = object.value(QStringLiteral("scope")).toString();
        field.desired = object.value(QStringLiteral("desired")).toVariant();
        field.effective = object.value(QStringLiteral("effective")).toVariant();
        field.requiresReconnect = strictBool(
            object, QStringLiteral("requires_reconnect")) ||
            strictBool(object, QStringLiteral("requires_relaunch"));
        result.fields.insert(it.key(), std::move(field));
    }

    const QJsonObject desired = root.value(QStringLiteral("desired")).toObject();
    const QJsonObject effective = root.value(QStringLiteral("effective")).toObject();
    const QJsonObject tuning = root.value(QStringLiteral("tuning")).toObject();
    const QJsonObject encoder = root.value(QStringLiteral("encoder")).toObject();
    result.adaptiveBitrateEnabled = strictOptionalBool(
        tuning, QStringLiteral("adaptive_bitrate_enabled"));
    if (!result.adaptiveBitrateEnabled.has_value()) {
        result.adaptiveBitrateEnabled = strictOptionalBool(
            effective, QStringLiteral("adaptive_bitrate_enabled"));
    }
    if (!result.adaptiveBitrateEnabled.has_value()) {
        result.adaptiveBitrateEnabled = strictOptionalBool(
            desired, QStringLiteral("adaptive_bitrate_enabled"));
    }
    result.aiAutoQualityEnabled = strictOptionalBool(
        tuning, QStringLiteral("ai_auto_quality_enabled"));
    if (!result.aiAutoQualityEnabled.has_value()) {
        result.aiAutoQualityEnabled = strictOptionalBool(
            effective, QStringLiteral("ai_auto_quality_enabled"));
    }
    if (!result.aiAutoQualityEnabled.has_value()) {
        result.aiAutoQualityEnabled = strictOptionalBool(
            desired, QStringLiteral("ai_auto_quality_enabled"));
    }
    result.aiOptimizerEnabled = strictOptionalBool(
        tuning, QStringLiteral("ai_optimizer_enabled"));
    if (!result.aiOptimizerEnabled.has_value()) {
        result.aiOptimizerEnabled = strictOptionalBool(
            effective, QStringLiteral("ai_optimizer_enabled"));
    }
    if (!result.aiOptimizerEnabled.has_value()) {
        result.aiOptimizerEnabled = strictOptionalBool(
            desired, QStringLiteral("ai_optimizer_enabled"));
    }
    result.adaptiveTargetBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_target_bitrate_kbps"));
    if (!result.adaptiveTargetBitrateKbps.has_value()) {
        result.adaptiveTargetBitrateKbps = strictPositiveInt(
            effective, QStringLiteral("adaptive_target_bitrate_kbps"));
    }
    if (!result.adaptiveTargetBitrateKbps.has_value()) {
        result.adaptiveTargetBitrateKbps = strictPositiveInt(
            effective, QStringLiteral("target_bitrate_kbps"));
    }
    result.adaptiveBaseBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_base_bitrate_kbps"));
    if (!result.adaptiveBaseBitrateKbps.has_value()) {
        result.adaptiveBaseBitrateKbps = strictPositiveInt(
            desired, QStringLiteral("target_bitrate_kbps"));
    }
    result.adaptiveMinBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_min_bitrate_kbps"));
    result.adaptiveMaxBitrateKbps = strictPositiveInt(
        tuning, QStringLiteral("adaptive_max_bitrate_kbps"));
    result.encoderBitrateKbps = strictPositiveInt(
        encoder, QStringLiteral("bitrate_kbps"));
    if (tuning.value(QStringLiteral("adaptive_bitrate_state")).isString()) {
        result.adaptiveState = tuning.value(
            QStringLiteral("adaptive_bitrate_state")).toString();
    }
    else if (root.value(QStringLiteral("adaptive_bitrate_state")).isString()) {
        result.adaptiveState = root.value(
            QStringLiteral("adaptive_bitrate_state")).toString();
    }
    if (tuning.value(QStringLiteral("adaptive_bitrate_reason")).isString()) {
        result.adaptiveReason = tuning.value(
            QStringLiteral("adaptive_bitrate_reason")).toString();
    }
    else if (root.value(QStringLiteral("adaptive_bitrate_reason")).isString()) {
        result.adaptiveReason = root.value(
            QStringLiteral("adaptive_bitrate_reason")).toString();
    }

    return result;
}

PolarisAvailability PolarisModels::availability(
    const PolarisDiscoverySnapshot& snapshot, PolarisOperation operation)
{
    if (!snapshot.complete) {
        return disabled(PolarisAvailabilityCode::Pending,
                        QStringLiteral("discovery_pending"),
                        QStringLiteral("Polaris discovery is in progress"));
    }
    if (const auto failure = dependencyFailure(snapshot.errorCode)) {
        return *failure;
    }
    if (snapshot.standardHost ||
            !operationCapabilityAvailable(snapshot, operation)) {
        return disabled(PolarisAvailabilityCode::CapabilityNotAdvertised,
                        QStringLiteral("capability_not_advertised"),
                        QStringLiteral(
                            "This Polaris version does not advertise this feature"));
    }
    if (!snapshot.session.valid) {
        const QString errorCode = snapshot.session.errorCode.isEmpty()
            ? QStringLiteral("malformed_response")
            : snapshot.session.errorCode;
        return *dependencyFailure(errorCode);
    }
    if ((operation == PolarisOperation::BitrateControl ||
         operation == PolarisOperation::AdaptiveQualityControl) &&
            !snapshot.settings.valid) {
        const QString errorCode = snapshot.settings.errorCode.isEmpty()
            ? QStringLiteral("malformed_response")
            : snapshot.settings.errorCode;
        return *dependencyFailure(errorCode);
    }
    if (operation == PolarisOperation::StopSession &&
            !snapshot.session.controls.stopEndpoint.usable) {
        return disabled(PolarisAvailabilityCode::CapabilityNotAdvertised,
                        QStringLiteral("capability_not_advertised"),
                        QStringLiteral(
                            "This Polaris version does not advertise this feature"));
    }
    if (operation == PolarisOperation::NamedCommand &&
            !snapshot.capabilities.commandCatalogValid) {
        const QString errorCode =
            snapshot.capabilities.commandCatalogErrorCode.isEmpty()
            ? QStringLiteral("malformed_response")
            : snapshot.capabilities.commandCatalogErrorCode;
        return *dependencyFailure(errorCode);
    }
    if (!operationPermissionGranted(snapshot, operation)) {
        return disabled(PolarisAvailabilityCode::PermissionDenied,
                        QStringLiteral("permission_denied"),
                        QStringLiteral("This paired client lacks permission"));
    }
    if (requiresControlOwnership(operation) &&
            (!snapshot.session.controllingClient ||
             !snapshot.session.ownsSession)) {
        return disabled(PolarisAvailabilityCode::NotControllingClient,
                        QStringLiteral("not_controlling_client"),
                        QStringLiteral("Only the controlling client can do this"));
    }
    if (snapshot.session.transitioning) {
        return disabled(PolarisAvailabilityCode::Transitioning,
                        QStringLiteral("session_transitioning"),
                        QStringLiteral("The session is transitioning"));
    }
    if (requiresSessionToken(operation) && !snapshot.session.tokenValid) {
        return disabled(PolarisAvailabilityCode::SessionTokenUnavailable,
                        QStringLiteral("session_token_unavailable"),
                        QStringLiteral(
                            "Current session token is unavailable"));
    }
    return {true, PolarisAvailabilityCode::Available, {}, {}};
}
