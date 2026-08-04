#pragma once

#include <QObject>

class QSettings;

class MoonlightSettingsImport final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool decisionRequired READ decisionRequired
               NOTIFY decisionRequiredChanged)

public:
    MoonlightSettingsImport(QSettings& legacySettings,
                            QSettings& perigeeSettings,
                            QObject* parent = nullptr);

    bool decisionRequired() const;

    Q_INVOKABLE bool acceptImport();
    Q_INVOKABLE bool declineImport();

signals:
    void decisionRequiredChanged();

private:
    bool destinationIsInitialized() const;
    bool legacyHasImportableData() const;
    bool recoverInterruptedTransaction();
    bool beginTransaction(QSettings& settings);
    bool syncPayloadPhase(QSettings& settings);
    bool completeTransaction(QSettings& settings, const QString& decision);
    void enterRecoveryMode();

    QSettings& m_LegacySettings;
    QSettings& m_PerigeeSettings;
    bool m_RecoveryRequired = false;
};
