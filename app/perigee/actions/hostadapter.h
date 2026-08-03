#pragma once

#include "actiontypes.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>

#include <functional>

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
                         const QVariantMap& parameters,
                         Completion completion) = 0;
    virtual void cancel(const QString& resourceKey) = 0;
};
