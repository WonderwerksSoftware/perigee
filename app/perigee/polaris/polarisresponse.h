#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QString>

struct PolarisResponse
{
    int httpStatus = 0;
    QByteArray body;
    QJsonDocument json;
    QString errorCode;
    QString userMessage;
    bool authenticated = false;
};
