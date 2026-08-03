#pragma once

#include "actiontypes.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>

#include <functional>
#include <optional>
#include <utility>

class ActionRegistry;

class ActionInvocation
{
public:
    explicit ActionInvocation(QVariantMap parameters = {})
        : m_Parameters(std::move(parameters))
    {
    }
    ActionInvocation(const ActionInvocation&) = delete;
    ActionInvocation& operator=(const ActionInvocation&) = delete;
    ActionInvocation(ActionInvocation&&) = default;
    ActionInvocation& operator=(ActionInvocation&&) = default;

    const QVariantMap& parameters() const { return m_Parameters; }
    bool confirmationGrantedFor(const QString& actionId) const
    {
        return !m_ConfirmedActionId.isEmpty() && m_ConfirmedActionId == actionId;
    }

private:
    friend class ActionRegistry;

    ActionInvocation(QVariantMap parameters,
                     QString confirmedActionId,
                     ActionState confirmedState)
        : m_Parameters(std::move(parameters))
        , m_ConfirmedActionId(std::move(confirmedActionId))
        , m_ConfirmedState(std::move(confirmedState))
    {
    }

    QVariantMap m_Parameters;
    QString m_ConfirmedActionId;
    std::optional<ActionState> m_ConfirmedState;
};

struct HostSnapshot {
    QSet<QString> advertisedCapabilities;
    quint32 grantedPermissions = 0;
    QHash<QString, ActionState> actionStates;
};

class HostAdapter
{
public:
    using Completion = std::function<void(const ActionResult&)>;

    virtual ~HostAdapter() = default;

    virtual HostSnapshot snapshot() = 0;
    virtual void cancel(const QString& resourceKey) = 0;

private:
    friend class ActionRegistry;

    virtual void execute(const QString& actionId,
                         QVariantMap parameters,
                         Completion completion) = 0;
};
