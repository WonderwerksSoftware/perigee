#pragma once

#include "actionlistmodel.h"

#include <QObject>
#include <QStringList>

class ActionRegistry;

class DeckController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isOpen READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool searchFocused READ searchFocused NOTIFY focusModeChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int activeCategory READ activeCategory NOTIFY activeCategoryChanged)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)
    Q_PROPERTY(ActionListModel* actionModel READ actionModel CONSTANT)
    Q_PROPERTY(bool confirmationVisible READ confirmationVisible NOTIFY confirmationChanged)
    Q_PROPERTY(QString confirmationActionLabel READ confirmationActionLabel NOTIFY confirmationChanged)

public:
    explicit DeckController(ActionRegistry* registry = nullptr,
                            QObject* parent = nullptr);

    bool isOpen() const;
    bool searchFocused() const;
    QString searchText() const;
    int activeCategory() const;
    QStringList categories() const;
    ActionListModel* actionModel();
    const ActionListModel* actionModel() const;
    bool confirmationVisible() const;
    QString confirmationActionLabel() const;

    Q_INVOKABLE void openFromKeyboard();
    Q_INVOKABLE void openFromController();
    Q_INVOKABLE void close();
    Q_INVOKABLE void setSearchText(const QString& searchText);
    Q_INVOKABLE void selectCategory(int categoryIndex);
    Q_INVOKABLE void nextCategory();
    Q_INVOKABLE void previousCategory();
    Q_INVOKABLE void focusSearch();
    Q_INVOKABLE void focusActions();
    Q_INVOKABLE void focusAction(const QString& actionId);
    Q_INVOKABLE void moveActionFocus(int delta);
    Q_INVOKABLE void activateFocusedAction();
    Q_INVOKABLE void cancelConfirmation();
    Q_INVOKABLE void acceptConfirmation();
    Q_INVOKABLE void back();
    Q_INVOKABLE void refresh();

signals:
    void openChanged();
    void searchTextChanged();
    void activeCategoryChanged();
    void focusModeChanged();
    void confirmationChanged();

private:
    static ActionCategory categoryForIndex(int categoryIndex);
    void setSearchFocus(bool focused);
    void clearConfirmation();
    void executeAction(const QString& actionId);

    ActionRegistry* m_Registry;
    ActionListModel m_ActionModel;
    bool m_IsOpen = false;
    bool m_SearchFocused = true;
    QString m_SearchText;
    int m_ActiveCategory = 0;
    QString m_ConfirmationActionId;
    QString m_ConfirmationActionLabel;
};
