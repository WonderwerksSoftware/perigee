#include "productidentity.h"

#include <QCoreApplication>

void ProductIdentity::apply()
{
    QCoreApplication::setOrganizationName(
        QStringLiteral("Perigee Streaming Project"));
    QCoreApplication::setOrganizationDomain(
        QStringLiteral("perigee-stream.app"));
    QCoreApplication::setApplicationName(QStringLiteral("Perigee"));
}

QString ProductIdentity::windowTitle()
{
    return QStringLiteral("Perigee");
}
