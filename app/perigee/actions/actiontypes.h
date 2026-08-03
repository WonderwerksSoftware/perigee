#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

enum class ActionCategory { Display, Input, Clipboard, Stats, Window, Session };
enum class ActionPhase { Idle, AwaitingConfirmation, Working, Succeeded, Failed };
enum class ConfirmationPolicy { Never, Always, WhenDisruptive };

struct ActionDescriptor {
    QString id;
    QString label;
    ActionCategory category;
    QStringList aliases;
    QString resourceKey;
    QString requiredCapability;
    quint32 requiredPermissions = 0;
    ConfirmationPolicy confirmation = ConfirmationPolicy::Never;
};

struct ActionState {
    bool visible = true;
    bool enabled = false;
    QVariant value;
    QString disabledReason;
    ActionPhase phase = ActionPhase::Idle;
    QString message;
};

struct ActionResult {
    bool ok = false;
    QString evidence;
    QString errorCode;
    QString userMessage;
};
