#pragma once

#include "hostadapter.h"

#include <QVector>

#include <memory>

class ActionRegistry
{
public:
    // The adapter is non-owning and must outlive this registry.
    ActionRegistry(QVector<ActionDescriptor> descriptors, HostAdapter& adapter);
    ActionRegistry(const ActionRegistry&) = delete;
    ActionRegistry& operator=(const ActionRegistry&) = delete;
    ActionRegistry(ActionRegistry&&) = delete;
    ActionRegistry& operator=(ActionRegistry&&) = delete;

    QVector<ActionDescriptor> actions(ActionCategory category) const;
    QVector<ActionDescriptor> search(const QString& query) const;
    ActionState state(const QString& actionId);
    bool requiresConfirmation(const QString& actionId) const;
    bool beginConfirmation(const QString& actionId);
    void cancelConfirmation();
    void acceptConfirmation(const QString& actionId,
                            const QVariantMap& parameters,
                            HostAdapter::Completion completion);
    void execute(const QString& actionId,
                 const QVariantMap& parameters,
                 HostAdapter::Completion completion);

private:
    struct RuntimeState;

    void executeInvocation(const QString& actionId,
                           ActionInvocation invocation,
                           HostAdapter::Completion completion);

    QVector<ActionDescriptor> m_Descriptors;
    HostAdapter& m_Adapter;
    std::shared_ptr<RuntimeState> m_RuntimeState;
};
