#pragma once

#include "hostadapter.h"
#include "sessionfacade.h"

#include <QPointer>
#include <QVector>

class GameStreamAdapter final : public HostAdapter
{
public:
    explicit GameStreamAdapter(SessionFacade* session);

    static QVector<ActionDescriptor> descriptors();

    HostSnapshot snapshot() override;
    void cancel(const QString& resourceKey) override;

private:
    friend class PolarisAdapter;

    void execute(const QString& actionId,
                 QVariantMap parameters,
                 Completion completion) override;
    SessionFacade* session() const;

    QPointer<SessionFacade> m_Session;
};
