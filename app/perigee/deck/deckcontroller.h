#pragma once

#include "actionlistmodel.h"
#include "perigee/input/controllerlayout.h"

#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class ActionRegistry;

class DeckController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isOpen READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool searchFocused READ searchFocused NOTIFY focusModeChanged)
    Q_PROPERTY(bool textInputRequested READ textInputRequested NOTIFY textInputRequestedChanged)
    Q_PROPERTY(FocusRegion focusRegion READ focusRegion NOTIFY focusModeChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int activeCategory READ activeCategory NOTIFY activeCategoryChanged)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)
    Q_PROPERTY(ActionListModel* actionModel READ actionModel CONSTANT)
    Q_PROPERTY(ControllerLayout* controllerLayout READ controllerLayout CONSTANT)
    Q_PROPERTY(bool controllerConnected READ controllerConnected WRITE setControllerConnected NOTIFY controllerConnectedChanged)
    Q_PROPERTY(bool confirmationVisible READ confirmationVisible NOTIFY confirmationChanged)
    Q_PROPERTY(QString confirmationActionLabel READ confirmationActionLabel NOTIFY confirmationChanged)
    Q_PROPERTY(QString confirmationMessage READ confirmationMessage NOTIFY confirmationChanged)

public:
    enum FocusRegion {
        SearchRegion,
        CategoriesRegion,
        ActionsRegion,
    };
    Q_ENUM(FocusRegion)

    explicit DeckController(ActionRegistry* registry = nullptr,
                            QObject* parent = nullptr);

    bool isOpen() const;
    bool searchFocused() const;
    bool textInputRequested() const;
    FocusRegion focusRegion() const;
    QString searchText() const;
    int activeCategory() const;
    QStringList categories() const;
    ActionListModel* actionModel();
    const ActionListModel* actionModel() const;
    ControllerLayout* controllerLayout();
    const ControllerLayout* controllerLayout() const;
    bool controllerConnected() const;
    bool confirmationVisible() const;
    QString confirmationActionLabel() const;
    QString confirmationMessage() const;

    Q_INVOKABLE void openFromKeyboard();
    Q_INVOKABLE void openFromController();
    void setControllerLayout(ControllerLayout::Family family,
                             bool swapFaceButtons);
    void setControllerConnected(bool connected);
    Q_INVOKABLE void close();
    Q_INVOKABLE void setSearchText(const QString& searchText);
    Q_INVOKABLE void selectCategory(int categoryIndex);
    Q_INVOKABLE void nextCategory();
    Q_INVOKABLE void previousCategory();
    Q_INVOKABLE void focusSearch();
    Q_INVOKABLE void focusCategories();
    Q_INVOKABLE void focusActions();
    Q_INVOKABLE void focusAction(const QString& actionId);
    Q_INVOKABLE void focusActionWithoutActivation(const QString& actionId);
    Q_INVOKABLE void moveActionFocus(int delta);
    Q_INVOKABLE void activateAction(const QString& actionId);
    Q_INVOKABLE void activateFocusedAction();
    Q_INVOKABLE void cancelConfirmation();
    Q_INVOKABLE void acceptConfirmation();
    Q_INVOKABLE void back();
    Q_INVOKABLE void refresh();
    bool pumpPendingWork();

signals:
    void openChanged();
    void searchTextChanged();
    void activeCategoryChanged();
    void focusModeChanged();
    void textInputRequestedChanged();
    void confirmationChanged();
    void controllerConnectedChanged();

private:
    static ActionCategory categoryForIndex(int categoryIndex);
    void setFocusRegion(FocusRegion region);
    void clearConfirmation();
    void executeAction(const QString& actionId);

    ActionRegistry* m_Registry;
    ActionListModel m_ActionModel;
    ControllerLayout m_ControllerLayout;
    bool m_ControllerConnected = false;
    bool m_IsOpen = false;
    FocusRegion m_FocusRegion = SearchRegion;
    QString m_SearchText;
    int m_ActiveCategory = 0;
    QString m_ConfirmationActionId;
    QString m_ConfirmationActionLabel;
    QString m_ConfirmationMessage;
    std::shared_ptr<std::atomic_bool> m_PendingRefresh;
};
