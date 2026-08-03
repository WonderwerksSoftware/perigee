#include "polarismodels.h"

#include "polarisapiclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

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
    endpoint.advertised = value.toString();
    endpoint.url = PolarisApiClient::resolveAdvertisedEndpoint(
        origin, endpoint.advertised, &endpoint.errorCode);
    endpoint.usable = endpoint.url.isValid();
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
            number < double(std::numeric_limits<qint64>::min()) ||
            number > double(std::numeric_limits<qint64>::max())) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
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
    case PolarisOperation::DisplaySwitch:
        return capabilities.features.contains(
            QStringLiteral("display_targets_v1")) &&
            capabilities.clientSettingsEndpoint.usable;
    }
    return false;
}

bool operationPermissionGranted(const PolarisDiscoverySnapshot& snapshot,
                                PolarisOperation operation)
{
    switch (operation) {
    case PolarisOperation::ClipboardRead:
        return snapshot.capabilities.clipboardReadAdvertised &&
            snapshot.session.controls.clipboardReadAllowed;
    case PolarisOperation::ClipboardWrite:
        return snapshot.capabilities.clipboardWriteAdvertised &&
            snapshot.session.controls.clipboardWriteAllowed;
    case PolarisOperation::NamedCommand:
        return snapshot.session.controls.commandsAllowed;
    case PolarisOperation::StopSession:
        return snapshot.session.controls.stopAllowed;
    case PolarisOperation::DisplaySwitch:
        return snapshot.session.controls.displaySelectionAllowed;
    }
    return false;
}

bool requiresControlOwnership(PolarisOperation operation)
{
    return operation == PolarisOperation::NamedCommand ||
        operation == PolarisOperation::StopSession ||
        operation == PolarisOperation::DisplaySwitch;
}

bool hasAlternateDisplay(const PolarisDiscoverySnapshot& snapshot)
{
    return std::any_of(snapshot.settings.targets.cbegin(),
                       snapshot.settings.targets.cend(),
                       [](const DisplayTarget& target) {
        return target.available && !target.current;
    });
}

}

QString DisplayTarget::stableKey() const
{
    return kind + QLatin1Char(':') + id;
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
    result.clipboardReadAdvertised = strictBool(
        clipboard, QStringLiteral("read"));
    result.clipboardWriteAdvertised = strictBool(
        clipboard, QStringLiteral("write"));
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

    const QJsonArray commands = namedCommands.value(
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
                !command.value(QStringLiteral("risk")).isString() ||
                !result.commandsEndpoint.usable) {
            continue;
        }
        NamedCommandMetadata metadata;
        metadata.index = static_cast<int>(index);
        metadata.identifier = command.value(QStringLiteral("id")).isString()
            ? command.value(QStringLiteral("id")).toString()
            : QString::number(index);
        metadata.displayName = command.value(QStringLiteral("name")).toString();
        metadata.risk = command.value(QStringLiteral("risk")).toString();
        metadata.endpoint = result.commandsEndpoint;
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
    result.controls.commandsAllowed = strictBool(
        controls, QStringLiteral("quit_allowed")) &&
        strictBool(controls, QStringLiteral("client_commands_enabled")) &&
        strictBool(controls, QStringLiteral("device_commands_enabled"));
    result.controls.clipboardReadAllowed = strictBool(
        controls, QStringLiteral("clipboard_read_allowed"));
    result.controls.clipboardWriteAllowed = strictBool(
        controls, QStringLiteral("clipboard_write_allowed"));
    result.controls.displaySelectionAllowed = strictBool(
        controls, QStringLiteral("display_selection_allowed"));
    result.controls.stopEndpoint = parseEndpoint(
        pairedOrigin, controls.value(QStringLiteral("stop_endpoint")));
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
    QJsonObject fields = root.value(QStringLiteral("fields")).toObject();
    if (fields.isEmpty()) {
        fields = root.value(QStringLiteral("sync_status")).toObject()
            .value(QStringLiteral("fields")).toObject();
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

    const QJsonObject capabilities = root.value(
        QStringLiteral("capabilities")).toObject();
    const QString currentMode = root.value(QStringLiteral("effective"))
        .toObject().value(QStringLiteral("stream_display_mode")).toString();
    QSet<QString> stableKeys;
    const QJsonArray modes = capabilities.value(QStringLiteral("modes")).toArray();
    for (const QJsonValue& value : modes) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject mode = value.toObject();
        if (!mode.value(QStringLiteral("value")).isString() ||
                mode.value(QStringLiteral("value")).toString().isEmpty() ||
                !mode.value(QStringLiteral("label")).isString() ||
                !mode.value(QStringLiteral("available")).isBool()) {
            continue;
        }
        DisplayTarget target;
        target.kind = QStringLiteral("stream-mode");
        target.id = mode.value(QStringLiteral("value")).toString();
        target.label = mode.value(QStringLiteral("label")).toString();
        target.available = mode.value(QStringLiteral("available")).toBool();
        target.current = !currentMode.isEmpty() && target.id == currentMode;
        target.requiresReconnect = strictBool(
            mode, QStringLiteral("restart_required"), true);
        target.unavailableReason = mode.value(
            QStringLiteral("unavailable_reason")).toString();
        if (!stableKeys.contains(target.stableKey())) {
            stableKeys.insert(target.stableKey());
            result.targets.push_back(std::move(target));
        }
    }

    const QString currentOutput = root.value(QStringLiteral("effective"))
        .toObject().value(QStringLiteral("output_name")).toString();
    const QJsonArray outputs = capabilities.value(QStringLiteral("outputs")).toArray();
    int currentOutputs = 0;
    const qsizetype outputStart = result.targets.size();
    for (const QJsonValue& value : outputs) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject output = value.toObject();
        if (!output.value(QStringLiteral("id")).isString() ||
                output.value(QStringLiteral("id")).toString().isEmpty() ||
                !output.value(QStringLiteral("label")).isString() ||
                !output.value(QStringLiteral("connected")).isBool()) {
            continue;
        }
        DisplayTarget target;
        target.kind = QStringLiteral("output");
        target.id = output.value(QStringLiteral("id")).toString();
        target.label = output.value(QStringLiteral("label")).toString();
        target.available = output.value(QStringLiteral("connected")).toBool();
        target.current = output.value(QStringLiteral("active")).isBool()
            ? output.value(QStringLiteral("active")).toBool()
            : (!currentOutput.isEmpty() && target.id == currentOutput);
        target.requiresReconnect = strictBool(
            output, QStringLiteral("requires_reconnect"), true);
        target.unavailableReason = output.value(
            QStringLiteral("unavailable_reason")).toString();
        if (!target.available && target.unavailableReason.isEmpty()) {
            target.unavailableReason = QStringLiteral("Display is disconnected");
        }
        if (!stableKeys.contains(target.stableKey())) {
            stableKeys.insert(target.stableKey());
            currentOutputs += target.current ? 1 : 0;
            result.targets.push_back(std::move(target));
        }
    }
    if (currentOutputs > 1) {
        result.currentConflict = true;
        for (qsizetype i = outputStart; i < result.targets.size(); ++i) {
            result.targets[i].current = false;
        }
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
    if (operation == PolarisOperation::StopSession &&
            !snapshot.session.controls.stopEndpoint.usable) {
        return disabled(PolarisAvailabilityCode::CapabilityNotAdvertised,
                        QStringLiteral("capability_not_advertised"),
                        QStringLiteral(
                            "This Polaris version does not advertise this feature"));
    }
    if (operation == PolarisOperation::DisplaySwitch &&
            !snapshot.settings.valid) {
        const QString errorCode = snapshot.settings.errorCode.isEmpty()
            ? QStringLiteral("malformed_response")
            : snapshot.settings.errorCode;
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
    if (operation == PolarisOperation::DisplaySwitch &&
            !hasAlternateDisplay(snapshot)) {
        return disabled(PolarisAvailabilityCode::NoAlternateDisplay,
                        QStringLiteral("no_alternate_display"),
                        QStringLiteral("No alternate display is available"));
    }
    return {true, PolarisAvailabilityCode::Available, {}, {}};
}
