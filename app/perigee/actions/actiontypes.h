#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

#include <optional>

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
    QString confirmationMessage;
};

struct ActionState {
    bool visible = true;
    bool enabled = false;
    bool disruptive = false;
    QVariant value;
    QString disabledCode;
    QString disabledReason;
    ActionPhase phase = ActionPhase::Idle;
    QString message;
};

struct ActionResult {
    bool ok = false;
    QString evidence;
    QString errorCode;
    QString userMessage;
    std::optional<ActionState> observedState = std::nullopt;
};
