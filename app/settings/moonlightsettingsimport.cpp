#include "moonlightsettingsimport.h"

#include <QSettings>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace {

constexpr auto DecisionKey = "migration/moonlightImportDecision";
constexpr auto TransactionPhaseKey =
    "migration/moonlightImportTransactionPhase";
constexpr auto PreparedPhase = "prepared";
constexpr auto PayloadSyncedPhase = "payload-synced";
constexpr auto CompletePhase = "complete";

const QStringList& preferenceKeys()
{
    static const QStringList keys {
        QStringLiteral("width"),
        QStringLiteral("height"),
        QStringLiteral("fps"),
        QStringLiteral("bitrate"),
        QStringLiteral("unlockbitrate"),
        QStringLiteral("autoadjustbitrate"),
        QStringLiteral("fullscreen"),
        QStringLiteral("vsync"),
        QStringLiteral("gameopts"),
        QStringLiteral("hostaudio"),
        QStringLiteral("multicontroller"),
        QStringLiteral("audiocfg"),
        QStringLiteral("videocfg"),
        QStringLiteral("hdr"),
        QStringLiteral("yuv444"),
        QStringLiteral("videodec"),
        QStringLiteral("windowmode"),
        QStringLiteral("mdns"),
        QStringLiteral("quitAppAfter"),
        QStringLiteral("mouseacceleration"),
        QStringLiteral("abstouchmode"),
        QStringLiteral("startwindowed"),
        QStringLiteral("framepacing"),
        QStringLiteral("connwarnings"),
        QStringLiteral("confwarnings"),
        QStringLiteral("uidisplaymode"),
        QStringLiteral("richpresence"),
        QStringLiteral("gamepadmouse"),
        QStringLiteral("defaultver"),
        QStringLiteral("packetsize"),
        QStringLiteral("detectnetblocking"),
        QStringLiteral("showperfoverlay"),
        QStringLiteral("swapmousebuttons"),
        QStringLiteral("muteonfocusloss"),
        QStringLiteral("backgroundgamepad"),
        QStringLiteral("reversescroll"),
        QStringLiteral("swapfacebuttons"),
        QStringLiteral("capturesyskeys"),
        QStringLiteral("keepawake"),
        QStringLiteral("language"),
        QStringLiteral("renderer"),
    };
    return keys;
}

const QStringList& hostKeys()
{
    static const QStringList keys {
        QStringLiteral("hostname"),
        QStringLiteral("customname"),
        QStringLiteral("uuid"),
        QStringLiteral("mac"),
        QStringLiteral("localaddress"),
        QStringLiteral("localport"),
        QStringLiteral("remoteaddress"),
        QStringLiteral("remoteport"),
        QStringLiteral("ipv6address"),
        QStringLiteral("ipv6port"),
        QStringLiteral("manualaddress"),
        QStringLiteral("manualport"),
        QStringLiteral("srvcert"),
        QStringLiteral("nvidiasw"),
    };
    return keys;
}

const QStringList& appKeys()
{
    static const QStringList keys {
        QStringLiteral("name"),
        QStringLiteral("id"),
        QStringLiteral("hdr"),
        QStringLiteral("appcollector"),
        QStringLiteral("hidden"),
        QStringLiteral("directlaunch"),
    };
    return keys;
}

struct ImportedHost {
    QVariantMap values;
    QVector<QVariantMap> apps;
};

bool hasCompletePairingIdentity(const QSettings& settings)
{
    return !settings.value(QStringLiteral("certificate")).toByteArray().isEmpty()
        && !settings.value(QStringLiteral("key")).toByteArray().isEmpty()
        && !settings.value(QStringLiteral("uniqueid")).toString().isEmpty();
}

QVector<ImportedHost> readPairedHosts(QSettings& settings)
{
    QVector<ImportedHost> hosts;
    if (!hasCompletePairingIdentity(settings)) {
        return hosts;
    }

    const int hostCount = settings.beginReadArray(QStringLiteral("hosts"));
    for (int hostIndex = 0; hostIndex < hostCount; ++hostIndex) {
        settings.setArrayIndex(hostIndex);
        ImportedHost host;
        for (const QString& key : hostKeys()) {
            if (settings.contains(key)) {
                host.values.insert(key, settings.value(key));
            }
        }

        const int appCount = settings.beginReadArray(QStringLiteral("apps"));
        for (int appIndex = 0; appIndex < appCount; ++appIndex) {
            settings.setArrayIndex(appIndex);
            QVariantMap app;
            for (const QString& key : appKeys()) {
                if (settings.contains(key)) {
                    app.insert(key, settings.value(key));
                }
            }
            if (app.value(QStringLiteral("id")).toInt() != 0
                    && !app.value(QStringLiteral("name")).toString().isNull()) {
                host.apps.append(app);
            }
        }
        settings.endArray();

        if (!host.values.value(QStringLiteral("uuid")).toString().isEmpty()
                && !host.values.value(QStringLiteral("srvcert")).toByteArray().isEmpty()) {
            hosts.append(host);
        }
    }
    settings.endArray();
    return hosts;
}

void writePairedHosts(QSettings& settings, const QVector<ImportedHost>& hosts)
{
    settings.beginWriteArray(QStringLiteral("hosts"));
    for (int hostIndex = 0; hostIndex < hosts.size(); ++hostIndex) {
        settings.setArrayIndex(hostIndex);
        const ImportedHost& host = hosts.at(hostIndex);
        for (auto value = host.values.cbegin(); value != host.values.cend(); ++value) {
            settings.setValue(value.key(), value.value());
        }

        settings.beginWriteArray(QStringLiteral("apps"));
        for (int appIndex = 0; appIndex < host.apps.size(); ++appIndex) {
            settings.setArrayIndex(appIndex);
            const QVariantMap& app = host.apps.at(appIndex);
            for (auto value = app.cbegin(); value != app.cend(); ++value) {
                settings.setValue(value.key(), value.value());
            }
        }
        settings.endArray();
    }
    settings.endArray();
}

}

MoonlightSettingsImport::MoonlightSettingsImport(QSettings& legacySettings,
                                                 QSettings& perigeeSettings,
                                                 QObject* parent)
    : QObject(parent),
      m_LegacySettings(legacySettings),
      m_PerigeeSettings(perigeeSettings)
{
    if (m_PerigeeSettings.contains(
            QString::fromLatin1(TransactionPhaseKey))) {
        m_RecoveryRequired = true;
        recoverInterruptedTransaction();
    }
}

bool MoonlightSettingsImport::destinationIsInitialized() const
{
    return !m_PerigeeSettings.allKeys().isEmpty();
}

bool MoonlightSettingsImport::legacyHasImportableData() const
{
    for (const QString& key : preferenceKeys()) {
        if (m_LegacySettings.contains(key)) {
            return true;
        }
    }

    return !readPairedHosts(m_LegacySettings).isEmpty();
}

bool MoonlightSettingsImport::decisionRequired() const
{
    return m_RecoveryRequired
        || (!m_PerigeeSettings.contains(QString::fromLatin1(DecisionKey))
        && !destinationIsInitialized()
        && legacyHasImportableData());
}

void MoonlightSettingsImport::enterRecoveryMode()
{
    m_RecoveryRequired = true;
}

bool MoonlightSettingsImport::recoverInterruptedTransaction()
{
    if (!m_RecoveryRequired
            && !m_PerigeeSettings.contains(
                QString::fromLatin1(TransactionPhaseKey))) {
        return true;
    }

    m_RecoveryRequired = true;
    QSettings recoverySettings(m_PerigeeSettings.fileName(),
                               m_PerigeeSettings.format());
    recoverySettings.clear();
    recoverySettings.sync();
    if (recoverySettings.status() != QSettings::NoError) {
        return false;
    }

    m_RecoveryRequired = false;
    return true;
}

bool MoonlightSettingsImport::beginTransaction(QSettings& settings)
{
    settings.setValue(QString::fromLatin1(TransactionPhaseKey),
                      QString::fromLatin1(PreparedPhase));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        enterRecoveryMode();
        return false;
    }

    m_RecoveryRequired = true;
    return true;
}

bool MoonlightSettingsImport::syncPayloadPhase(QSettings& settings)
{
    settings.setValue(QString::fromLatin1(TransactionPhaseKey),
                      QString::fromLatin1(PayloadSyncedPhase));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        enterRecoveryMode();
        return false;
    }
    return true;
}

bool MoonlightSettingsImport::completeTransaction(QSettings& settings,
                                                  const QString& decision)
{
    settings.setValue(QString::fromLatin1(DecisionKey), decision);
    settings.setValue(QString::fromLatin1(TransactionPhaseKey),
                      QString::fromLatin1(CompletePhase));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        enterRecoveryMode();
        return false;
    }

    settings.remove(QString::fromLatin1(TransactionPhaseKey));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        enterRecoveryMode();
        return false;
    }

    m_RecoveryRequired = false;
    emit decisionRequiredChanged();
    return true;
}

bool MoonlightSettingsImport::acceptImport()
{
    if (m_RecoveryRequired && !recoverInterruptedTransaction()) {
        return false;
    }
    if (!decisionRequired()) {
        return false;
    }

    QSettings transactionSettings(m_PerigeeSettings.fileName(),
                                  m_PerigeeSettings.format());
    if (!beginTransaction(transactionSettings)) {
        return false;
    }

    for (const QString& key : preferenceKeys()) {
        if (m_LegacySettings.contains(key)) {
            transactionSettings.setValue(key, m_LegacySettings.value(key));
        }
    }

    const QVector<ImportedHost> hosts = readPairedHosts(m_LegacySettings);
    if (!hosts.isEmpty()) {
        writePairedHosts(transactionSettings, hosts);
        transactionSettings.setValue(
            QStringLiteral("certificate"),
            m_LegacySettings.value(QStringLiteral("certificate")));
        transactionSettings.setValue(
            QStringLiteral("key"),
            m_LegacySettings.value(QStringLiteral("key")));
        transactionSettings.setValue(
            QStringLiteral("uniqueid"),
            m_LegacySettings.value(QStringLiteral("uniqueid")));
    }

    if (!syncPayloadPhase(transactionSettings)) {
        return false;
    }
    return completeTransaction(transactionSettings, QStringLiteral("imported"));
}

bool MoonlightSettingsImport::declineImport()
{
    if (m_RecoveryRequired && !recoverInterruptedTransaction()) {
        return false;
    }
    if (!decisionRequired()) {
        return false;
    }
    QSettings transactionSettings(m_PerigeeSettings.fileName(),
                                  m_PerigeeSettings.format());
    if (!beginTransaction(transactionSettings)
            || !syncPayloadPhase(transactionSettings)) {
        return false;
    }
    return completeTransaction(transactionSettings, QStringLiteral("declined"));
}
