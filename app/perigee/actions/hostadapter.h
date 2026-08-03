#pragma once

#include "actiontypes.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>

#include <functional>
#include <utility>

class ActionRegistry;

class ActionInvocation
{
public:
    explicit ActionInvocation(QVariantMap parameters = {})
        : m_Parameters(std::move(parameters))
    {
    }

    const QVariantMap& parameters() const { return m_Parameters; }
    bool confirmationGrantedFor(const QString& actionId) const
    {
        return !m_ConfirmedActionId.isEmpty() && m_ConfirmedActionId == actionId;
    }

private:
    friend class ActionRegistry;

    ActionInvocation(QVariantMap parameters, QString confirmedActionId)
        : m_Parameters(std::move(parameters))
        , m_ConfirmedActionId(std::move(confirmedActionId))
    {
    }

    QVariantMap m_Parameters;
    QString m_ConfirmedActionId;
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
    virtual void execute(const QString& actionId,
                         const ActionInvocation& invocation,
                         Completion completion) = 0;
    virtual void cancel(const QString& resourceKey) = 0;
};
