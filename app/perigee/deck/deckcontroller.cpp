#include "deckcontroller.h"

#include "perigee/actions/actionregistry.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

DeckController::DeckController(ActionRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_Registry(registry)
    , m_ActionModel(registry, this)
{
}

bool DeckController::isOpen() const
{
    return m_IsOpen;
}

bool DeckController::searchFocused() const
{
    return m_SearchFocused;
}

QString DeckController::searchText() const
{
    return m_SearchText;
}

int DeckController::activeCategory() const
{
    return m_ActiveCategory;
}

QStringList DeckController::categories() const
{
    return {
        QStringLiteral("Display"),
        QStringLiteral("Input"),
        QStringLiteral("Clipboard"),
        QStringLiteral("Stats"),
        QStringLiteral("Window"),
        QStringLiteral("Session"),
    };
}

ActionListModel* DeckController::actionModel()
{
    return &m_ActionModel;
}

const ActionListModel* DeckController::actionModel() const
{
    return &m_ActionModel;
}

bool DeckController::confirmationVisible() const
{
    return !m_ConfirmationActionId.isEmpty();
}

QString DeckController::confirmationActionLabel() const
{
    return m_ConfirmationActionLabel;
}

void DeckController::openFromKeyboard()
{
    clearConfirmation();
    if (m_ActiveCategory != 0) {
        m_ActiveCategory = 0;
        emit activeCategoryChanged();
    }
    if (!m_SearchText.isEmpty()) {
        m_SearchText.clear();
        emit searchTextChanged();
    }
    m_ActionModel.setSearchText({});
    m_ActionModel.setCategory(ActionCategory::Display);
    m_ActionModel.refresh();
    m_ActionModel.clearFocus();
    setSearchFocus(true);
    if (!m_IsOpen) {
        m_IsOpen = true;
        emit openChanged();
    }
}

void DeckController::openFromController()
{
    openFromKeyboard();
    focusActions();
}

void DeckController::close()
{
    clearConfirmation();
    if (!m_IsOpen) {
        return;
    }
    m_IsOpen = false;
    emit openChanged();
}

void DeckController::setSearchText(const QString& searchText)
{
    if (m_SearchText == searchText) {
        return;
    }
    clearConfirmation();
    m_SearchText = searchText;
    m_ActionModel.setSearchText(searchText);
    emit searchTextChanged();
}

void DeckController::selectCategory(int categoryIndex)
{
    const int categoryCount = categories().size();
    if (categoryCount == 0) {
        return;
    }
    const int normalized =
        (categoryIndex % categoryCount + categoryCount) % categoryCount;
    clearConfirmation();
    if (!m_SearchText.isEmpty()) {
        m_SearchText.clear();
        m_ActionModel.setSearchText({});
        emit searchTextChanged();
    }
    if (m_ActiveCategory != normalized) {
        m_ActiveCategory = normalized;
        emit activeCategoryChanged();
    }
    m_ActionModel.setCategory(categoryForIndex(normalized));
    setSearchFocus(false);
    m_ActionModel.focusFirstEnabled();
}

void DeckController::nextCategory()
{
    selectCategory(m_ActiveCategory + 1);
}

void DeckController::previousCategory()
{
    selectCategory(m_ActiveCategory - 1);
}

void DeckController::focusSearch()
{
    clearConfirmation();
    m_ActionModel.clearFocus();
    setSearchFocus(true);
}

void DeckController::focusActions()
{
    if (m_ActionModel.focusedActionId().isEmpty()) {
        m_ActionModel.focusFirstEnabled();
    }
    setSearchFocus(false);
}

void DeckController::focusAction(const QString& actionId)
{
    if (m_ActionModel.focusAction(actionId)) {
        setSearchFocus(false);
    }
}

void DeckController::moveActionFocus(int delta)
{
    clearConfirmation();
    if (m_ActionModel.moveFocus(delta)) {
        setSearchFocus(false);
    }
}

void DeckController::activateFocusedAction()
{
    if (!m_ActionModel.focusedActionEnabled()) {
        return;
    }
    if (m_ActionModel.focusedActionRequiresConfirmation()) {
        m_ConfirmationActionId = m_ActionModel.focusedActionId();
        m_ConfirmationActionLabel = m_ActionModel.focusedActionLabel();
        m_ActionModel.setAwaitingConfirmation(m_ConfirmationActionId);
        emit confirmationChanged();
        return;
    }
    executeAction(m_ActionModel.focusedActionId());
}

void DeckController::cancelConfirmation()
{
    clearConfirmation();
}

void DeckController::acceptConfirmation()
{
    const QString actionId = m_ConfirmationActionId;
    if (actionId.isEmpty()) {
        return;
    }
    clearConfirmation();
    executeAction(actionId);
}

void DeckController::back()
{
    if (confirmationVisible()) {
        cancelConfirmation();
        return;
    }
    if (!m_SearchFocused) {
        focusSearch();
        return;
    }
    if (!m_SearchText.isEmpty()) {
        setSearchText({});
        return;
    }
    close();
}

void DeckController::refresh()
{
    m_ActionModel.refresh();
    if (!m_SearchFocused && m_ActionModel.focusedActionId().isEmpty()) {
        m_ActionModel.focusFirstEnabled();
    }
    if (confirmationVisible() &&
            m_ActionModel.focusedActionId() != m_ConfirmationActionId) {
        clearConfirmation();
    }
}

ActionCategory DeckController::categoryForIndex(int categoryIndex)
{
    switch (categoryIndex) {
    case 0:
        return ActionCategory::Display;
    case 1:
        return ActionCategory::Input;
    case 2:
        return ActionCategory::Clipboard;
    case 3:
        return ActionCategory::Stats;
    case 4:
        return ActionCategory::Window;
    case 5:
        return ActionCategory::Session;
    default:
        return ActionCategory::Display;
    }
}

void DeckController::setSearchFocus(bool focused)
{
    if (m_SearchFocused == focused) {
        return;
    }
    m_SearchFocused = focused;
    emit focusModeChanged();
}

void DeckController::clearConfirmation()
{
    if (m_ConfirmationActionId.isEmpty()) {
        return;
    }
    m_ConfirmationActionId.clear();
    m_ConfirmationActionLabel.clear();
    m_ActionModel.setAwaitingConfirmation({});
    emit confirmationChanged();
}

void DeckController::executeAction(const QString& actionId)
{
    if (m_Registry == nullptr || actionId.isEmpty()) {
        return;
    }

    QPointer<DeckController> self(this);
    m_Registry->execute(actionId, {}, [self](const ActionResult&) {
        if (self.isNull()) {
            return;
        }
        if (QThread::currentThread() == self->thread()) {
            self->refresh();
        }
        else {
            QMetaObject::invokeMethod(self.data(), [self] {
                if (!self.isNull()) {
                    self->refresh();
                }
            }, Qt::QueuedConnection);
        }
    });
    refresh();
}
