#include "deckcontroller.h"

#include "perigee/actions/actionregistry.h"

DeckController::DeckController(ActionRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_Registry(registry)
    , m_ActionModel(registry, this)
    , m_PendingRefresh(std::make_shared<std::atomic_bool>(false))
{
}

bool DeckController::isOpen() const
{
    return m_IsOpen;
}

bool DeckController::searchFocused() const
{
    return m_FocusRegion == SearchRegion;
}

bool DeckController::textInputRequested() const
{
    return m_IsOpen && m_FocusRegion == SearchRegion;
}

DeckController::FocusRegion DeckController::focusRegion() const
{
    return m_FocusRegion;
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

QString DeckController::confirmationMessage() const
{
    return m_ConfirmationMessage;
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
    setFocusRegion(SearchRegion);
    if (!m_IsOpen) {
        m_IsOpen = true;
        emit openChanged();
        emit textInputRequestedChanged();
    }
}

void DeckController::openFromController()
{
    openFromKeyboard();
    for (int categoryIndex = 0; categoryIndex < categories().size(); ++categoryIndex) {
        selectCategory(categoryIndex);
        if (m_ActionModel.hasEnabledAction()) {
            focusActions();
            return;
        }
    }
    selectCategory(0);
    focusSearch();
}

void DeckController::close()
{
    clearConfirmation();
    if (!m_IsOpen) {
        return;
    }
    m_IsOpen = false;
    emit openChanged();
    if (m_FocusRegion == SearchRegion) {
        emit textInputRequestedChanged();
    }
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
    if (m_FocusRegion == SearchRegion) {
        setFocusRegion(CategoriesRegion);
    }
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
    setFocusRegion(SearchRegion);
}

void DeckController::focusCategories()
{
    clearConfirmation();
    setFocusRegion(CategoriesRegion);
}

void DeckController::focusActions()
{
    if (m_ActionModel.focusedActionId().isEmpty()) {
        m_ActionModel.focusFirstEnabled();
    }
    setFocusRegion(ActionsRegion);
}

void DeckController::focusAction(const QString& actionId)
{
    const QString previousActionId = m_ActionModel.focusedActionId();
    if (m_ActionModel.focusAction(actionId)) {
        if (previousActionId != actionId) {
            clearConfirmation();
        }
        setFocusRegion(ActionsRegion);
    }
}

void DeckController::moveActionFocus(int delta)
{
    clearConfirmation();
    if (m_ActionModel.moveFocus(delta)) {
        setFocusRegion(ActionsRegion);
    }
}

void DeckController::activateAction(const QString& actionId)
{
    focusAction(actionId);
    if (m_ActionModel.focusedActionId() != actionId ||
            !m_ActionModel.focusedActionEnabled()) {
        return;
    }
    activateFocusedAction();
}

void DeckController::activateFocusedAction()
{
    if (!m_ActionModel.focusedActionEnabled()) {
        return;
    }
    if (m_ActionModel.focusedActionRequiresConfirmation()) {
        if (m_Registry == nullptr ||
                !m_Registry->beginConfirmation(
                    m_ActionModel.focusedActionId())) {
            refresh();
            return;
        }
        m_ConfirmationActionId = m_ActionModel.focusedActionId();
        m_ConfirmationActionLabel = m_ActionModel.focusedActionLabel();
        m_ConfirmationMessage =
            m_ActionModel.focusedActionConfirmationMessage();
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
    if (actionId.isEmpty() || m_Registry == nullptr) {
        return;
    }
    m_ConfirmationActionId.clear();
    m_ConfirmationActionLabel.clear();
    m_ConfirmationMessage.clear();
    m_ActionModel.setAwaitingConfirmation({});
    emit confirmationChanged();

    std::weak_ptr<std::atomic_bool> pendingRefresh = m_PendingRefresh;
    m_Registry->acceptConfirmation(
        actionId, {}, [pendingRefresh](const ActionResult&) {
            if (const auto pending = pendingRefresh.lock()) {
                pending->store(true, std::memory_order_release);
            }
        });
    refresh();
}

void DeckController::back()
{
    if (confirmationVisible()) {
        cancelConfirmation();
        return;
    }
    if (m_FocusRegion == ActionsRegion) {
        focusCategories();
        return;
    }
    if (m_FocusRegion == CategoriesRegion) {
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
    if (confirmationVisible()) {
        clearConfirmation();
    }
    m_ActionModel.refresh();
    if (m_FocusRegion == ActionsRegion &&
            m_ActionModel.focusedActionId().isEmpty()) {
        m_ActionModel.focusFirstEnabled();
    }
}

bool DeckController::pumpPendingWork()
{
    if (!m_PendingRefresh->exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    refresh();
    return true;
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

void DeckController::setFocusRegion(FocusRegion region)
{
    if (m_FocusRegion == region) {
        return;
    }
    const bool previouslyRequested = textInputRequested();
    m_FocusRegion = region;
    emit focusModeChanged();
    if (previouslyRequested != textInputRequested()) {
        emit textInputRequestedChanged();
    }
}

void DeckController::clearConfirmation()
{
    if (m_Registry != nullptr) {
        m_Registry->cancelConfirmation();
    }
    if (m_ConfirmationActionId.isEmpty()) {
        return;
    }
    m_ConfirmationActionId.clear();
    m_ConfirmationActionLabel.clear();
    m_ConfirmationMessage.clear();
    m_ActionModel.setAwaitingConfirmation({});
    emit confirmationChanged();
}

void DeckController::executeAction(const QString& actionId)
{
    if (m_Registry == nullptr || actionId.isEmpty()) {
        return;
    }

    std::weak_ptr<std::atomic_bool> pendingRefresh = m_PendingRefresh;
    m_Registry->execute(actionId, {},
                        [pendingRefresh](const ActionResult&) {
        if (const auto pending = pendingRefresh.lock()) {
            pending->store(true, std::memory_order_release);
        }
    });
    refresh();
}
