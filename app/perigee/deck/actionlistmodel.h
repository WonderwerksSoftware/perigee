#pragma once

#include "perigee/actions/actiontypes.h"

#include <QAbstractListModel>
#include <QVector>

class ActionRegistry;

class ActionListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int focusedRow READ focusedRow NOTIFY focusedRowChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        LabelRole,
        CategoryRole,
        ValueTextRole,
        EnabledRole,
        DisabledReasonRole,
        PhaseRole,
        MessageRole,
        FocusedRole,
        RequiresConfirmationRole,
    };
    Q_ENUM(Role)

    explicit ActionListModel(ActionRegistry* registry = nullptr,
                             QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRegistry(ActionRegistry* registry);
    void setCategory(ActionCategory category);
    void setSearchText(const QString& searchText);
    void refresh();

    QString focusedActionId() const;
    int focusedRow() const;
    QString focusedActionLabel() const;
    QString focusedActionConfirmationMessage() const;
    bool focusedActionEnabled() const;
    bool focusedActionRequiresConfirmation() const;
    bool hasEnabledAction() const;
    void clearFocus();
    bool focusFirstEnabled();
    bool moveFocus(int delta);
    bool focusAction(const QString& actionId);
    void setAwaitingConfirmation(const QString& actionId);

signals:
    void focusedRowChanged();

private:
    struct Row {
        ActionDescriptor descriptor;
        ActionState state;
        bool requiresConfirmation = false;
    };

    int rowForId(const QString& actionId) const;
    int nextEnabledRow(int startRow, int delta) const;
    void changeFocusedAction(const QString& actionId);
    static QString categoryName(ActionCategory category);
    static QString phaseName(ActionPhase phase);

    ActionRegistry* m_Registry;
    ActionCategory m_Category = ActionCategory::Display;
    QString m_SearchText;
    QVector<Row> m_Rows;
    QString m_FocusedActionId;
    QString m_AwaitingConfirmationId;
};
