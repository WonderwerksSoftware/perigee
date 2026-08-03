#pragma once

#include "hostadapter.h"

#include <QVector>

#include <memory>

class ActionRegistry
{
public:
    ActionRegistry(QVector<ActionDescriptor> descriptors, HostAdapter& adapter);

    QVector<ActionDescriptor> actions(ActionCategory category) const;
    QVector<ActionDescriptor> search(const QString& query) const;
    ActionState state(const QString& actionId);
    bool requiresConfirmation(const QString& actionId, bool disruptive) const;
    void execute(const QString& actionId,
                 const QVariantMap& parameters,
                 HostAdapter::Completion completion);

private:
    struct RuntimeState;

    QVector<ActionDescriptor> m_Descriptors;
    HostAdapter& m_Adapter;
    std::shared_ptr<RuntimeState> m_RuntimeState;
};
