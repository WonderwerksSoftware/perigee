#pragma once

#include "polarisresponse.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVector>

#include <optional>

struct AdvertisedPolarisEndpoint
{
    QString advertised;
    QUrl url;
    bool usable = false;
    QString errorCode;
};

struct NamedCommandMetadata
{
    int index = -1;
    QString identifier;
    QString displayName;
    QString risk;
    AdvertisedPolarisEndpoint endpoint;
    bool retainsRawCommand = false;
};

struct PolarisCommandCatalog
{
    bool valid = false;
    QString errorCode;
    QVector<NamedCommandMetadata> commands;
};

struct PolarisCapabilities
{
    bool valid = false;
    bool isPolarisContract = false;
    bool standardHost = false;
    QString errorCode;
    QSet<QString> features;
    AdvertisedPolarisEndpoint sessionStatusEndpoint;
    AdvertisedPolarisEndpoint clientSettingsEndpoint;
    AdvertisedPolarisEndpoint commandsEndpoint;
    bool clipboardLimitValid = true;
    qint64 maxClipboardTextBytes = 1024 * 1024;
    bool commandCatalogValid = false;
    QString commandCatalogErrorCode;
    QVector<NamedCommandMetadata> commands;
};

struct PolarisControls
{
    bool hostTuningAllowed = false;
    bool stopAllowed = false;
    bool commandsAllowed = false;
    bool clipboardReadAllowed = false;
    bool clipboardWriteAllowed = false;
    AdvertisedPolarisEndpoint stopEndpoint;
};

struct PolarisSessionStatus
{
    bool valid = false;
    QString errorCode;
    QString state;
    QString sessionToken;
    bool tokenValid = false;
    QString role;
    bool controllingClient = false;
    bool ownsSession = false;
    bool transitioning = false;
    PolarisControls controls;
    std::optional<bool> adaptiveBitrateEnabled;
    std::optional<bool> aiAutoQualityEnabled;
    std::optional<bool> aiOptimizerEnabled;
    std::optional<int> adaptiveTargetBitrateKbps;
    std::optional<int> adaptiveBaseBitrateKbps;
    std::optional<int> adaptiveMinBitrateKbps;
    std::optional<int> adaptiveMaxBitrateKbps;
    std::optional<int> encoderBitrateKbps;
    QString adaptiveState;
    QString adaptiveReason;
};

struct ClientSettingField
{
    QString name;
    QString direction;
    QString scope;
    QVariant desired;
    QVariant effective;
    bool requiresReconnect = false;
};

struct PolarisClientSettings
{
    bool valid = false;
    QString errorCode;
    QHash<QString, ClientSettingField> fields;
    std::optional<bool> adaptiveBitrateEnabled;
    std::optional<bool> aiAutoQualityEnabled;
    std::optional<bool> aiOptimizerEnabled;
    std::optional<int> adaptiveTargetBitrateKbps;
    std::optional<int> adaptiveBaseBitrateKbps;
    std::optional<int> adaptiveMinBitrateKbps;
    std::optional<int> adaptiveMaxBitrateKbps;
    std::optional<int> encoderBitrateKbps;
    QString adaptiveState;
    QString adaptiveReason;
};

struct PolarisDiscoverySnapshot
{
    quint64 generation = 0;
    bool complete = false;
    bool standardHost = false;
    QString errorCode;
    PolarisCapabilities capabilities;
    PolarisSessionStatus session;
    PolarisClientSettings settings;
};

enum class PolarisOperation {
    ClipboardRead,
    ClipboardWrite,
    NamedCommand,
    StopSession,
    BitrateControl,
    AdaptiveQualityControl,
};

enum class PolarisAvailabilityCode {
    Available,
    Pending,
    Unreachable,
    TlsIdentityMismatch,
    AuthenticationFailed,
    MalformedResponse,
    CapabilityNotAdvertised,
    PermissionDenied,
    NotControllingClient,
    Transitioning,
    SessionTokenUnavailable,
};

struct PolarisAvailability
{
    bool enabled = false;
    PolarisAvailabilityCode code = PolarisAvailabilityCode::Pending;
    QString errorCode;
    QString reason;
};

namespace PolarisModels {

inline constexpr qint64 AbsoluteClipboardTextCeiling = 1024 * 1024;

PolarisCapabilities parseCapabilities(const PolarisResponse& response,
                                      const QUrl& pairedOrigin);
PolarisCommandCatalog parseCommands(
    const PolarisResponse& response,
    const AdvertisedPolarisEndpoint& validatedEndpoint);
PolarisSessionStatus parseSessionStatus(const PolarisResponse& response,
                                        const QUrl& pairedOrigin);
PolarisClientSettings parseClientSettings(const PolarisResponse& response);
PolarisAvailability availability(const PolarisDiscoverySnapshot& snapshot,
                                  PolarisOperation operation);

}
