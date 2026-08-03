#include "actionlistmodel.h"

#include "perigee/actions/actionregistry.h"

#include <QtGlobal>

#include <utility>

ActionListModel::ActionListModel(ActionRegistry* registry, QObject* parent)
    : QAbstractListModel(parent)
    , m_Registry(registry)
{
    refresh();
}

int ActionListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_Rows.size();
}

QVariant ActionListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.column() != 0 ||
            index.row() < 0 || index.row() >= m_Rows.size()) {
        return {};
    }

    const Row& row = m_Rows.at(index.row());
    switch (role) {
    case IdRole:
        return row.descriptor.id;
    case LabelRole:
        return row.descriptor.label;
    case CategoryRole:
        return categoryName(row.descriptor.category);
    case ValueTextRole:
        return row.state.value.toString();
    case EnabledRole:
        return row.state.enabled;
    case DisabledReasonRole:
        return row.state.disabledReason;
    case PhaseRole:
        return phaseName(row.descriptor.id == m_AwaitingConfirmationId
                             ? ActionPhase::AwaitingConfirmation
                             : row.state.phase);
    case MessageRole:
        return row.state.message;
    case FocusedRole:
        return row.descriptor.id == m_FocusedActionId;
    case RequiresConfirmationRole:
        return row.requiresConfirmation;
    default:
        return {};
    }
}

QHash<int, QByteArray> ActionListModel::roleNames() const
{
    return {
        { IdRole, "id" },
        { LabelRole, "label" },
        { CategoryRole, "category" },
        { ValueTextRole, "valueText" },
        { EnabledRole, "enabled" },
        { DisabledReasonRole, "disabledReason" },
        { PhaseRole, "phase" },
        { MessageRole, "message" },
        { FocusedRole, "focused" },
        { RequiresConfirmationRole, "requiresConfirmation" },
    };
}

void ActionListModel::setRegistry(ActionRegistry* registry)
{
    if (m_Registry == registry) {
        return;
    }
    m_Registry = registry;
    m_FocusedActionId.clear();
    m_AwaitingConfirmationId.clear();
    refresh();
}

void ActionListModel::setCategory(ActionCategory category)
{
    if (m_Category == category && m_SearchText.isEmpty()) {
        return;
    }
    m_Category = category;
    refresh();
}

void ActionListModel::setSearchText(const QString& searchText)
{
    if (m_SearchText == searchText) {
        return;
    }
    m_SearchText = searchText;
    refresh();
}

void ActionListModel::refresh()
{
    const QString previousFocus = m_FocusedActionId;
    const int previousRow = rowForId(previousFocus);

    QVector<Row> rows;
    if (m_Registry != nullptr) {
        const QVector<ActionDescriptor> descriptors = m_SearchText.trimmed().isEmpty()
            ? m_Registry->actions(m_Category)
            : m_Registry->search(m_SearchText);
        rows.reserve(descriptors.size());
        for (const ActionDescriptor& descriptor : descriptors) {
            const ActionState state = m_Registry->state(descriptor.id);
            if (!state.visible) {
                continue;
            }
            rows.push_back({
                descriptor,
                state,
                m_Registry->requiresConfirmation(descriptor.id, true),
            });
        }
    }

    beginResetModel();
    m_Rows = std::move(rows);
    m_FocusedActionId.clear();

    const int retainedRow = rowForId(previousFocus);
    if (retainedRow >= 0 && m_Rows.at(retainedRow).state.enabled) {
        m_FocusedActionId = previousFocus;
    }
    else if (!previousFocus.isEmpty() && !m_Rows.isEmpty()) {
        const int fallbackStart = qBound(0, previousRow, m_Rows.size() - 1);
        const int fallbackRow = nextEnabledRow(fallbackStart, 1);
        if (fallbackRow >= 0) {
            m_FocusedActionId = m_Rows.at(fallbackRow).descriptor.id;
        }
    }
    if (rowForId(m_AwaitingConfirmationId) < 0) {
        m_AwaitingConfirmationId.clear();
    }
    endResetModel();
}

QString ActionListModel::focusedActionId() const
{
    return m_FocusedActionId;
}

QString ActionListModel::focusedActionLabel() const
{
    const int row = rowForId(m_FocusedActionId);
    return row >= 0 ? m_Rows.at(row).descriptor.label : QString();
}

bool ActionListModel::focusedActionEnabled() const
{
    const int row = rowForId(m_FocusedActionId);
    return row >= 0 && m_Rows.at(row).state.enabled;
}

bool ActionListModel::focusedActionRequiresConfirmation() const
{
    const int row = rowForId(m_FocusedActionId);
    return row >= 0 && m_Rows.at(row).requiresConfirmation;
}

void ActionListModel::clearFocus()
{
    changeFocusedAction({});
}

bool ActionListModel::focusFirstEnabled()
{
    const int row = nextEnabledRow(0, 1);
    if (row < 0) {
        changeFocusedAction({});
        return false;
    }
    changeFocusedAction(m_Rows.at(row).descriptor.id);
    return true;
}

bool ActionListModel::moveFocus(int delta)
{
    if (m_Rows.isEmpty() || delta == 0) {
        return false;
    }

    int startRow = rowForId(m_FocusedActionId);
    if (startRow < 0) {
        startRow = delta > 0 ? 0 : m_Rows.size() - 1;
    }
    else {
        startRow = (startRow + (delta > 0 ? 1 : -1) + m_Rows.size()) %
            m_Rows.size();
    }
    const int row = nextEnabledRow(startRow, delta > 0 ? 1 : -1);
    if (row < 0) {
        return false;
    }
    changeFocusedAction(m_Rows.at(row).descriptor.id);
    return true;
}

bool ActionListModel::focusAction(const QString& actionId)
{
    const int row = rowForId(actionId);
    if (row < 0 || !m_Rows.at(row).state.enabled) {
        return false;
    }
    changeFocusedAction(actionId);
    return true;
}

void ActionListModel::setAwaitingConfirmation(const QString& actionId)
{
    if (m_AwaitingConfirmationId == actionId) {
        return;
    }
    const int oldRow = rowForId(m_AwaitingConfirmationId);
    const int newRow = rowForId(actionId);
    m_AwaitingConfirmationId = actionId;
    if (oldRow >= 0) {
        emit dataChanged(index(oldRow, 0), index(oldRow, 0), { PhaseRole });
    }
    if (newRow >= 0 && newRow != oldRow) {
        emit dataChanged(index(newRow, 0), index(newRow, 0), { PhaseRole });
    }
}

int ActionListModel::rowForId(const QString& actionId) const
{
    if (actionId.isEmpty()) {
        return -1;
    }
    for (int row = 0; row < m_Rows.size(); ++row) {
        if (m_Rows.at(row).descriptor.id == actionId) {
            return row;
        }
    }
    return -1;
}

int ActionListModel::nextEnabledRow(int startRow, int delta) const
{
    if (m_Rows.isEmpty()) {
        return -1;
    }
    const int direction = delta >= 0 ? 1 : -1;
    int row = (startRow % m_Rows.size() + m_Rows.size()) % m_Rows.size();
    for (int visited = 0; visited < m_Rows.size(); ++visited) {
        if (m_Rows.at(row).state.enabled) {
            return row;
        }
        row = (row + direction + m_Rows.size()) % m_Rows.size();
    }
    return -1;
}

void ActionListModel::changeFocusedAction(const QString& actionId)
{
    if (m_FocusedActionId == actionId) {
        return;
    }
    const int oldRow = rowForId(m_FocusedActionId);
    const int newRow = rowForId(actionId);
    m_FocusedActionId = actionId;
    if (oldRow >= 0) {
        emit dataChanged(index(oldRow, 0), index(oldRow, 0), { FocusedRole });
    }
    if (newRow >= 0) {
        emit dataChanged(index(newRow, 0), index(newRow, 0), { FocusedRole });
    }
}

QString ActionListModel::categoryName(ActionCategory category)
{
    switch (category) {
    case ActionCategory::Display:
        return QStringLiteral("Display");
    case ActionCategory::Input:
        return QStringLiteral("Input");
    case ActionCategory::Clipboard:
        return QStringLiteral("Clipboard");
    case ActionCategory::Stats:
        return QStringLiteral("Stats");
    case ActionCategory::Window:
        return QStringLiteral("Window");
    case ActionCategory::Session:
        return QStringLiteral("Session");
    }
    return {};
}

QString ActionListModel::phaseName(ActionPhase phase)
{
    switch (phase) {
    case ActionPhase::Idle:
        return QStringLiteral("idle");
    case ActionPhase::AwaitingConfirmation:
        return QStringLiteral("awaitingConfirmation");
    case ActionPhase::Working:
        return QStringLiteral("working");
    case ActionPhase::Succeeded:
        return QStringLiteral("succeeded");
    case ActionPhase::Failed:
        return QStringLiteral("failed");
    }
    return {};
}
