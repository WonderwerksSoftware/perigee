#pragma once

#include "actiontypes.h"

#include <QString>
#include <QStringList>
#include <QVector>

class ActionCategories final
{
public:
    static QVector<ActionCategory> ordered();
    static QStringList displayNames();
    static QString displayName(ActionCategory category);
    static ActionCategory at(int index);
};
