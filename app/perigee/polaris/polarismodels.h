#pragma once

#include "polarisresponse.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVector>

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
    bool clipboardReadAdvertised = false;
    bool clipboardWriteAdvertised = false;
    bool clipboardLimitValid = true;
    qint64 maxClipboardTextBytes = 1024 * 1024;
    QVector<NamedCommandMetadata> commands;
};

struct PolarisControls
{
    bool stopAllowed = false;
    bool commandsAllowed = false;
    bool clipboardReadAllowed = false;
    bool clipboardWriteAllowed = false;
    bool displaySelectionAllowed = false;
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

struct DisplayTarget
{
    QString kind;
    QString id;
    QString label;
    bool available = false;
    bool current = false;
    bool requiresReconnect = false;
    QString unavailableReason;

    QString stableKey() const;
};

struct PolarisClientSettings
{
    bool valid = false;
    QString errorCode;
    QHash<QString, ClientSettingField> fields;
    QVector<DisplayTarget> targets;
    bool currentConflict = false;
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
    DisplaySwitch,
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
    NoAlternateDisplay,
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
PolarisSessionStatus parseSessionStatus(const PolarisResponse& response,
                                        const QUrl& pairedOrigin);
PolarisClientSettings parseClientSettings(const PolarisResponse& response);
PolarisAvailability availability(const PolarisDiscoverySnapshot& snapshot,
                                  PolarisOperation operation);

}
