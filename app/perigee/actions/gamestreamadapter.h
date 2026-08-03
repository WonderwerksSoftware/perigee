#pragma once

#include "hostadapter.h"

#include <QPointer>
#include <QVector>

class SessionFacade;

class GameStreamAdapter final : public HostAdapter
{
public:
    explicit GameStreamAdapter(SessionFacade* session);

    static QVector<ActionDescriptor> descriptors();

    HostSnapshot snapshot() override;
    void execute(const QString& actionId,
                 const QVariantMap& parameters,
                 Completion completion) override;
    void cancel(const QString& resourceKey) override;

private:
    SessionFacade* session() const;

    SessionFacade* m_Session;
    QPointer<QObject> m_LifetimeAuthority;
};
