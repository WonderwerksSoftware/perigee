#include "actioncategories.h"

#include <array>

namespace {

struct CategoryEntry {
    ActionCategory category;
    const char* displayName;
};

constexpr std::array<CategoryEntry, 7> Categories {{
    { ActionCategory::Display, "Display" },
    { ActionCategory::Quality, "Quality" },
    { ActionCategory::Input, "Input" },
    { ActionCategory::Clipboard, "Clipboard" },
    { ActionCategory::Stats, "Stats" },
    { ActionCategory::Window, "Window" },
    { ActionCategory::Session, "Session" },
}};

}

QVector<ActionCategory> ActionCategories::ordered()
{
    QVector<ActionCategory> result;
    result.reserve(int(Categories.size()));
    for (const CategoryEntry& entry : Categories) {
        result.push_back(entry.category);
    }
    return result;
}

QStringList ActionCategories::displayNames()
{
    QStringList result;
    result.reserve(int(Categories.size()));
    for (const CategoryEntry& entry : Categories) {
        result.push_back(QString::fromLatin1(entry.displayName));
    }
    return result;
}

QString ActionCategories::displayName(ActionCategory category)
{
    for (const CategoryEntry& entry : Categories) {
        if (entry.category == category) {
            return QString::fromLatin1(entry.displayName);
        }
    }
    return {};
}

ActionCategory ActionCategories::at(int index)
{
    return index >= 0 && index < int(Categories.size())
        ? Categories.at(std::size_t(index)).category
        : ActionCategory::Display;
}
