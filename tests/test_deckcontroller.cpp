#include "test_registry.h"

#include "perigee/actions/actionregistry.h"
#include "perigee/actions/actioncategories.h"
#include "perigee/actions/gamestreamadapter.h"
#include "perigee/deck/actionlistmodel.h"
#include "perigee/deck/deckcontroller.h"
#include "perigee/polaris/polarisadapter.h"

#include <QAbstractItemModel>
#include <QtTest>

#include <optional>
#include <thread>
#include <utility>

namespace {

ActionDescriptor descriptor(const QString& id,
                            const QString& label,
                            ActionCategory category,
                            ConfirmationPolicy confirmation = ConfirmationPolicy::Never,
                            const QString& confirmationMessage = {})
{
    return { id, label, category, {}, {}, {}, 0, confirmation,
             confirmationMessage };
}

ActionState availableState(const QString& value = {}, bool disruptive = false)
{
    ActionState state;
    state.enabled = true;
    state.value = value;
    state.disruptive = disruptive;
    return state;
}

class MutableHostAdapter final : public HostAdapter
{
public:
    HostSnapshot currentSnapshot;
    QStringList executedActionIds;
    QVector<QVariantMap> executedParameters;
    std::optional<ActionResult> synchronousResult;
    Completion pendingCompletion;

    HostSnapshot snapshot() override
    {
        return currentSnapshot;
    }

    void execute(const QString& actionId,
                 QVariantMap parameters,
                 Completion completion) override
    {
        executedActionIds.push_back(actionId);
        executedParameters.push_back(std::move(parameters));
        if (synchronousResult.has_value()) {
            completion(*synchronousResult);
        }
        else {
            pendingCompletion = std::move(completion);
        }
    }

    void cancel(const QString&) override
    {
    }
};

QString focusedActionId(const DeckController& controller)
{
    const auto* model = controller.actionModel();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, ActionListModel::FocusedRole).toBool()) {
            return model->data(index, ActionListModel::IdRole).toString();
        }
    }
    return {};
}

QString valueForAction(const DeckController& controller, const QString& actionId)
{
    const auto* model = controller.actionModel();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, ActionListModel::IdRole).toString() == actionId) {
            return model->data(index, ActionListModel::ValueTextRole).toString();
        }
    }
    return {};
}

QString displayActionId(const QString& kind, const QString& id)
{
    const QByteArray key = (kind + QLatin1Char(':') + id).toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QStringLiteral("display.target.%1").arg(QString::fromLatin1(key));
}

}

class DeckControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void keyboardOpenFocusesSearchAndShowsDisplayActions();
    void categoryCyclingMakesActionsReachableWithoutSearchText();
    void authoritativeCategoryOrderReachesEveryCoreAction();
    void productionCatalogIsReachableByCategoryWithoutSearch();
    void controllerOperatesPhysicalDisplaysAndKeepsDeckOpen();
    void controllerOpenPublishesEffectiveGlyphLayout();
    void refreshPreservesFocusedActionAndUpdatesItsState();
    void backUnwindsConfirmationActionsSearchAndDeck();
    void confirmationCanBeCancelledOrAccepted();
    void whenDisruptiveUsesAuthoritativeRiskInDeck();
    void refreshAndStateChangeInvalidateConfirmation();
    void disabledFocusedActionFallsBackToAnEnabledPeer();
    void controllerOpenSkipsEmptyDisplayCategory();
    void controllerOpenSkipsAllDisabledDisplayCategory();
    void controllerOpenWithoutEnabledActionsKeepsSearchFocus();
    void inFlightEmptyResourceActionExecutesOnlyOnce();
    void repeatedActivationDoesNotRedirectFromWorkingActionToPeer();
    void pointerFocusCancelsConfirmationOnlyAfterSuccessfulChange();
    void verifiedDisabledActionCanBeSelectedButNeverActivated();
    void textInputRequestTracksOpenSearchFocus();
    void deckPumpDrainsOnlyDeckCompletions();
};

void DeckControllerTest::keyboardOpenFocusesSearchAndShowsDisplayActions()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), availableState(QStringLiteral("Desk monitor")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.mouse-capture"), availableState(QStringLiteral("Captured")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Choose display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("input.mouse-capture"), QStringLiteral("Mouse capture"),
                   ActionCategory::Input),
    }, adapter);
    DeckController controller(&registry);

    controller.openFromKeyboard();

    QVERIFY(controller.isOpen());
    QVERIFY(controller.searchFocused());
    QCOMPARE(controller.activeCategory(), 0);
    QCOMPARE(controller.searchText(), QString());
    QCOMPARE(controller.actionModel()->rowCount(), 1);
    QCOMPARE(controller.actionModel()->data(
                 controller.actionModel()->index(0, 0), ActionListModel::IdRole).toString(),
             QStringLiteral("display.select"));
    QVERIFY(focusedActionId(controller).isEmpty());
}

void DeckControllerTest::categoryCyclingMakesActionsReachableWithoutSearchText()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.mouse-capture"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Choose display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("input.mouse-capture"), QStringLiteral("Mouse capture"),
                   ActionCategory::Input),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();

    controller.nextCategory();

    QCOMPARE(controller.activeCategory(), 1);
    QCOMPARE(controller.searchText(), QString());
    QVERIFY(!controller.searchFocused());
    QCOMPARE(focusedActionId(controller), QStringLiteral("input.mouse-capture"));

    controller.previousCategory();
    QCOMPARE(controller.activeCategory(), 0);
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.select"));
}

void DeckControllerTest::authoritativeCategoryOrderReachesEveryCoreAction()
{
    MutableHostAdapter adapter;
    QVector<ActionDescriptor> descriptors;
    const QVector<ActionCategory> expectedOrder {
        ActionCategory::Display,
        ActionCategory::Input,
        ActionCategory::Clipboard,
        ActionCategory::Stats,
        ActionCategory::Window,
        ActionCategory::Session,
    };
    const QStringList expectedNames {
        QStringLiteral("Display"),
        QStringLiteral("Input"),
        QStringLiteral("Clipboard"),
        QStringLiteral("Stats"),
        QStringLiteral("Window"),
        QStringLiteral("Session"),
    };
    const QStringList actionIds {
        QStringLiteral("display.select"),
        QStringLiteral("input.capture"),
        QStringLiteral("clipboard.send"),
        QStringLiteral("stats.toggle"),
        QStringLiteral("window.fullscreen"),
        QStringLiteral("session.disconnect"),
    };
    for (int index = 0; index < expectedOrder.size(); ++index) {
        descriptors.push_back(descriptor(
            actionIds.at(index),
            QStringLiteral("Core action %1").arg(index),
            expectedOrder.at(index)));
        adapter.currentSnapshot.actionStates.insert(
            actionIds.at(index), availableState());
    }
    ActionRegistry registry(descriptors, adapter);
    DeckController controller(&registry);
    controller.openFromController();

    QCOMPARE(ActionCategories::ordered(), expectedOrder);
    QCOMPARE(controller.categories(), expectedNames);
    for (int index = 0; index < expectedOrder.size(); ++index) {
        controller.selectCategory(index);
        QCOMPARE(controller.activeCategory(), index);
        QCOMPARE(controller.actionModel()->rowCount(), 1);
        QCOMPARE(focusedActionId(controller), actionIds.at(index));
        QCOMPARE(controller.actionModel()->data(
                     controller.actionModel()->index(0, 0),
                     ActionListModel::CategoryRole).toString(),
                 expectedNames.at(index));
    }
}

void DeckControllerTest::productionCatalogIsReachableByCategoryWithoutSearch()
{
    MutableHostAdapter adapter;
    const QString targetId = displayActionId(
        QStringLiteral("output"), QStringLiteral("DP-1"));
    ActionState targetState = availableState();
    targetState.value = QVariantMap {
        {QStringLiteral("kind"), QStringLiteral("output")},
        {QStringLiteral("id"), QStringLiteral("DP-1")},
        {QStringLiteral("label"), QStringLiteral("Desk monitor")},
        {QStringLiteral("available"), true},
        {QStringLiteral("current"), true},
        {QStringLiteral("requires_reconnect"), false},
        {QStringLiteral("unavailable_reason"), QString()},
    };
    adapter.currentSnapshot.actionStates.insert(targetId, targetState);

    ActionState commandState = availableState();
    commandState.value = QVariantMap {
        {QStringLiteral("index"), 0},
        {QStringLiteral("name"), QStringLiteral("Open terminal")},
        {QStringLiteral("risk"), QStringLiteral("safe")},
    };
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("host.command.0"), commandState);

    ActionState physicalStatus;
    physicalStatus.value = QStringLiteral("Unknown");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.physical-status"), physicalStatus);
    for (int displayNumber = 1; displayNumber <= 13; ++displayNumber) {
        ActionState physicalDisplay;
        physicalDisplay.visible = displayNumber <= 3;
        physicalDisplay.enabled = displayNumber <= 3;
        adapter.currentSnapshot.actionStates.insert(
            QStringLiteral("display.physical.%1").arg(displayNumber),
            physicalDisplay);
    }

    const QHash<QString, ActionCategory> expected {
        {QStringLiteral("input.mouse-capture"), ActionCategory::Input},
        {QStringLiteral("input.keyboard-capture"), ActionCategory::Input},
        {QStringLiteral("input.release-captured"), ActionCategory::Input},
        {QStringLiteral("stats.overlay"), ActionCategory::Stats},
        {QStringLiteral("window.fullscreen"), ActionCategory::Window},
        {QStringLiteral("session.disconnect-client"), ActionCategory::Session},
        {QStringLiteral("session.quit-perigee"), ActionCategory::Session},
        {targetId, ActionCategory::Display},
        {QStringLiteral("clipboard.send-local"), ActionCategory::Clipboard},
        {QStringLiteral("clipboard.fetch-remote"), ActionCategory::Clipboard},
        {QStringLiteral("session.end-host"), ActionCategory::Session},
        {QStringLiteral("host.command.0"), ActionCategory::Session},
        {QStringLiteral("display.physical-status"), ActionCategory::Display},
        {QStringLiteral("display.physical.1"), ActionCategory::Display},
        {QStringLiteral("display.physical.2"), ActionCategory::Display},
        {QStringLiteral("display.physical.3"), ActionCategory::Display},
    };
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        if (!adapter.currentSnapshot.actionStates.contains(it.key())) {
            adapter.currentSnapshot.actionStates.insert(
                it.key(), availableState());
        }
    }

    ActionRegistry registry(PolarisAdapter::descriptors(), adapter);
    DeckController controller(&registry);
    controller.openFromController();

    QSet<QString> reached;
    const QVector<ActionCategory> categories = ActionCategories::ordered();
    for (int categoryIndex = 0;
         categoryIndex < categories.size(); ++categoryIndex) {
        controller.selectCategory(categoryIndex);
        QCOMPARE(controller.searchText(), QString());
        for (int row = 0; row < controller.actionModel()->rowCount(); ++row) {
            const QModelIndex index = controller.actionModel()->index(row, 0);
            const QString id = controller.actionModel()->data(
                index, ActionListModel::IdRole).toString();
            QVERIFY2(expected.contains(id), qPrintable(id));
            QCOMPARE(expected.value(id), categories.at(categoryIndex));
            QCOMPARE(controller.actionModel()->data(
                         index, ActionListModel::CategoryRole).toString(),
                     ActionCategories::displayName(categories.at(categoryIndex)));
            reached.insert(id);
        }
    }
    QCOMPARE(reached, QSet<QString>(expected.keyBegin(), expected.keyEnd()));

    MutableHostAdapter fallbackAdapter;
    ActionState hiddenPhysicalState;
    hiddenPhysicalState.visible = false;
    fallbackAdapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.physical-status"), hiddenPhysicalState);
    for (int displayNumber = 1; displayNumber <= 13; ++displayNumber) {
        fallbackAdapter.currentSnapshot.actionStates.insert(
            QStringLiteral("display.physical.%1").arg(displayNumber),
            hiddenPhysicalState);
    }
    ActionRegistry fallbackRegistry(
        PolarisAdapter::descriptors(), fallbackAdapter);
    DeckController fallbackController(&fallbackRegistry);
    fallbackController.openFromKeyboard();
    QCOMPARE(fallbackController.actionModel()->rowCount(), 1);
    QCOMPARE(fallbackController.actionModel()->data(
                 fallbackController.actionModel()->index(0, 0),
                 ActionListModel::IdRole).toString(),
             QStringLiteral("display.switch"));
}

void DeckControllerTest::controllerOperatesPhysicalDisplaysAndKeepsDeckOpen()
{
    MutableHostAdapter adapter;
    ActionState status;
    status.value = QStringLiteral("Unknown");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.physical-status"), status);
    for (int displayNumber = 1; displayNumber <= 13; ++displayNumber) {
        ActionState displayState;
        displayState.visible = displayNumber <= 3;
        displayState.enabled = displayNumber <= 3;
        adapter.currentSnapshot.actionStates.insert(
            QStringLiteral("display.physical.%1").arg(displayNumber),
            displayState);
    }
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.disconnect-client"), availableState());

    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);
    DeckController controller(&registry);
    controller.openFromController();

    QCOMPARE(controller.activeCategory(), 0);
    QStringList reachedDisplays;
    for (int row = 0; row < controller.actionModel()->rowCount(); ++row) {
        const QModelIndex index = controller.actionModel()->index(row, 0);
        const QString id = controller.actionModel()->data(
            index, ActionListModel::IdRole).toString();
        if (id.startsWith(QStringLiteral("display.physical."))) {
            reachedDisplays.push_back(id);
        }
    }
    QCOMPARE(reachedDisplays,
             QStringList({QStringLiteral("display.physical.1"),
                          QStringLiteral("display.physical.2"),
                          QStringLiteral("display.physical.3")}));

    QCOMPARE(focusedActionId(controller),
             QStringLiteral("display.physical.1"));
    controller.moveActionFocus(1);
    QCOMPARE(focusedActionId(controller),
             QStringLiteral("display.physical.2"));
    controller.activateFocusedAction();
    QCOMPARE(adapter.executedActionIds,
             QStringList({QStringLiteral("display.physical.2")}));
    QVERIFY(controller.isOpen());

    auto successCompletion = std::move(adapter.pendingCompletion);
    successCompletion({true, QStringLiteral("Display 2 requested; video resumed"),
                       {}, {}});
    controller.pumpPendingWork();
    QVERIFY(controller.isOpen());

    controller.focusAction(QStringLiteral("display.physical.3"));
    controller.activateFocusedAction();
    auto failureCompletion = std::move(adapter.pendingCompletion);
    failureCompletion({false, {},
                       QStringLiteral("display_verification_timeout"),
                       QStringLiteral(
                           "Perigee did not receive fresh video after the display request.")});
    controller.pumpPendingWork();
    QVERIFY(controller.isOpen());

    controller.selectCategory(5);
    QCOMPARE(focusedActionId(controller),
             QStringLiteral("session.disconnect-client"));
}

void DeckControllerTest::controllerOpenPublishesEffectiveGlyphLayout()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"),
                   QStringLiteral("Choose display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);

    controller.setControllerLayout(ControllerLayout::Family::Nintendo, true);
    controller.openFromController();

    QCOMPARE(controller.controllerLayout()->family(),
             ControllerLayout::Family::Nintendo);
    QVERIFY(controller.controllerLayout()->swapFaceButtons());
    QCOMPARE(controller.controllerLayout()->confirmLabel(),
             QStringLiteral("A"));
    QCOMPARE(controller.controllerLayout()->backLabel(),
             QStringLiteral("B"));
    QCOMPARE(controller.controllerLayout()->searchLabel(),
             QStringLiteral("Y"));
}

void DeckControllerTest::refreshPreservesFocusedActionAndUpdatesItsState()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.first"), availableState(QStringLiteral("Left")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.second"), availableState(QStringLiteral("Right")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.first"), QStringLiteral("First display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.second"), QStringLiteral("Second display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.focusActions();
    controller.moveActionFocus(1);
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.second"));

    adapter.currentSnapshot.actionStates[QStringLiteral("display.second")].value =
        QStringLiteral("Studio display");
    controller.refresh();

    QCOMPARE(focusedActionId(controller), QStringLiteral("display.second"));
    QCOMPARE(valueForAction(controller, QStringLiteral("display.second")),
             QStringLiteral("Studio display"));
}

void DeckControllerTest::backUnwindsConfirmationActionsSearchAndDeck()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.end"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("session.end"), QStringLiteral("End host session"),
                   ActionCategory::Session, ConfirmationPolicy::Always,
                   QStringLiteral("End the host session for every connected client?")),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.setSearchText(QStringLiteral("session"));
    controller.focusActions();
    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());

    controller.back();
    QVERIFY(!controller.confirmationVisible());
    QVERIFY(controller.isOpen());
    QCOMPARE(focusedActionId(controller), QStringLiteral("session.end"));

    controller.back();
    QCOMPARE(controller.focusRegion(), DeckController::CategoriesRegion);
    QVERIFY(controller.isOpen());

    controller.back();
    QVERIFY(controller.searchFocused());
    QVERIFY(controller.isOpen());

    controller.back();
    QCOMPARE(controller.searchText(), QString());
    QVERIFY(controller.isOpen());

    controller.back();
    QVERIFY(!controller.isOpen());
}

void DeckControllerTest::confirmationCanBeCancelledOrAccepted()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.end"), availableState());
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("Host session ended"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("session.end"), QStringLiteral("End host session"),
                   ActionCategory::Session, ConfirmationPolicy::Always,
                   QStringLiteral("End the host session for every connected client?")),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.setSearchText(QStringLiteral("session"));
    controller.focusActions();

    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());
    QCOMPARE(controller.confirmationActionLabel(), QStringLiteral("End host session"));
    QCOMPARE(controller.confirmationMessage(),
             QStringLiteral("End the host session for every connected client?"));
    QVERIFY(adapter.executedActionIds.isEmpty());

    controller.cancelConfirmation();
    QVERIFY(!controller.confirmationVisible());
    QVERIFY(adapter.executedActionIds.isEmpty());

    controller.activateFocusedAction();
    controller.acceptConfirmation();
    QCOMPARE(adapter.executedActionIds,
             QStringList({ QStringLiteral("session.end") }));
    QCOMPARE(adapter.executedParameters, QVector<QVariantMap>({{}}));
    QVERIFY(!controller.confirmationVisible());
    QCOMPARE(valueForAction(controller, QStringLiteral("session.end")), QString());
}

void DeckControllerTest::whenDisruptiveUsesAuthoritativeRiskInDeck()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("command.run"), availableState(QStringLiteral("safe")));
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("Command accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("command.run"), QStringLiteral("Run command"),
                   ActionCategory::Session, ConfirmationPolicy::WhenDisruptive),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.setSearchText(QStringLiteral("command"));
    controller.focusActions();

    controller.activateFocusedAction();
    QVERIFY(!controller.confirmationVisible());
    QCOMPARE(adapter.executedActionIds,
             QStringList({QStringLiteral("command.run")}));

    adapter.currentSnapshot.actionStates[QStringLiteral("command.run")]
        = availableState(QStringLiteral("dangerous"), true);
    controller.refresh();
    controller.activateFocusedAction();

    QVERIFY(controller.confirmationVisible());
    QCOMPARE(adapter.executedActionIds.size(), 1);
}

void DeckControllerTest::refreshAndStateChangeInvalidateConfirmation()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.end"), availableState());
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("Session ended"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("session.end"), QStringLiteral("End session"),
                   ActionCategory::Session, ConfirmationPolicy::Always),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.setSearchText(QStringLiteral("session"));
    controller.focusActions();

    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());
    controller.refresh();
    QVERIFY(!controller.confirmationVisible());
    ActionResult result;
    registry.acceptConfirmation(
        QStringLiteral("session.end"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());

    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());
    ActionState disabled;
    disabled.disabledReason = QStringLiteral("Session is no longer available.");
    adapter.currentSnapshot.actionStates[QStringLiteral("session.end")] = disabled;
    controller.acceptConfirmation();
    QVERIFY(!controller.confirmationVisible());
    QVERIFY(adapter.executedActionIds.isEmpty());

    adapter.currentSnapshot.actionStates[QStringLiteral("session.end")] = availableState();
    registry.acceptConfirmation(
        QStringLiteral("session.end"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void DeckControllerTest::disabledFocusedActionFallsBackToAnEnabledPeer()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.first"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.second"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.first"), QStringLiteral("First display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.second"), QStringLiteral("Second display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.focusActions();
    controller.moveActionFocus(1);
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.second"));

    ActionState unavailable;
    unavailable.disabledReason = QStringLiteral("Display is no longer available.");
    adapter.currentSnapshot.actionStates[QStringLiteral("display.second")] = unavailable;
    controller.refresh();

    QCOMPARE(focusedActionId(controller), QStringLiteral("display.first"));
}

void DeckControllerTest::controllerOpenSkipsEmptyDisplayCategory()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.mouse-capture"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("input.mouse-capture"), QStringLiteral("Mouse capture"),
                   ActionCategory::Input),
    }, adapter);
    DeckController controller(&registry);

    controller.openFromController();

    QCOMPARE(controller.activeCategory(), 1);
    QVERIFY(!controller.searchFocused());
    QCOMPARE(focusedActionId(controller), QStringLiteral("input.mouse-capture"));
}

void DeckControllerTest::controllerOpenSkipsAllDisabledDisplayCategory()
{
    MutableHostAdapter adapter;
    ActionState unavailable;
    unavailable.disabledReason = QStringLiteral("Display is unavailable.");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.unavailable"), unavailable);
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("stats.overlay"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.unavailable"),
                   QStringLiteral("Unavailable display"), ActionCategory::Display),
        descriptor(QStringLiteral("stats.overlay"), QStringLiteral("Statistics overlay"),
                   ActionCategory::Stats),
    }, adapter);
    DeckController controller(&registry);

    controller.openFromController();

    QCOMPARE(controller.activeCategory(), 3);
    QVERIFY(!controller.searchFocused());
    QCOMPARE(focusedActionId(controller), QStringLiteral("stats.overlay"));
}

void DeckControllerTest::controllerOpenWithoutEnabledActionsKeepsSearchFocus()
{
    MutableHostAdapter adapter;
    ActionState unavailable;
    unavailable.disabledReason = QStringLiteral("Display is unavailable.");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.unavailable"), unavailable);
    ActionRegistry registry({
        descriptor(QStringLiteral("display.unavailable"),
                   QStringLiteral("Unavailable display"), ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);

    controller.openFromController();

    QCOMPARE(controller.activeCategory(), 0);
    QVERIFY(controller.searchFocused());
    QVERIFY(focusedActionId(controller).isEmpty());
}

void DeckControllerTest::inFlightEmptyResourceActionExecutesOnlyOnce()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.refresh"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.refresh"), QStringLiteral("Refresh display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();

    controller.activateFocusedAction();
    controller.activateFocusedAction();

    QCOMPARE(adapter.executedActionIds,
             QStringList({ QStringLiteral("display.refresh") }));
    const QModelIndex actionIndex = controller.actionModel()->index(0, 0);
    QCOMPARE(controller.actionModel()->data(
                 actionIndex, ActionListModel::PhaseRole).toString(),
             QStringLiteral("working"));
    QVERIFY(!controller.actionModel()->data(
        actionIndex, ActionListModel::EnabledRole).toBool());
}

void DeckControllerTest::repeatedActivationDoesNotRedirectFromWorkingActionToPeer()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.first"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.second"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.first"), QStringLiteral("First display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.second"), QStringLiteral("Second display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.first"));

    controller.activateFocusedAction();
    controller.activateFocusedAction();

    QCOMPARE(adapter.executedActionIds,
             QStringList({ QStringLiteral("display.first") }));
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.first"));
}

void DeckControllerTest::pointerFocusCancelsConfirmationOnlyAfterSuccessfulChange()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.confirm"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.other"), availableState());
    ActionState unavailable;
    unavailable.disabledReason = QStringLiteral("Display is unavailable.");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.disabled"), unavailable);
    ActionRegistry registry({
        descriptor(QStringLiteral("display.confirm"), QStringLiteral("Confirm display"),
                   ActionCategory::Display, ConfirmationPolicy::Always),
        descriptor(QStringLiteral("display.other"), QStringLiteral("Other display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.disabled"), QStringLiteral("Disabled display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());

    controller.focusAction(QStringLiteral("display.confirm"));
    QVERIFY(controller.confirmationVisible());
    controller.focusAction(QStringLiteral("display.disabled"));
    QVERIFY(controller.confirmationVisible());
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.confirm"));

    controller.focusAction(QStringLiteral("display.other"));
    QVERIFY(!controller.confirmationVisible());
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.other"));
    ActionResult result;
    registry.acceptConfirmation(
        QStringLiteral("display.confirm"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void DeckControllerTest::verifiedDisabledActionCanBeSelectedButNeverActivated()
{
    MutableHostAdapter adapter;
    ActionState current;
    current.enabled = false;
    current.disabledCode = QStringLiteral("current_target");
    current.disabledReason = QStringLiteral("This display is currently active.");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.current"), current);
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.other"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.current"), QStringLiteral("Current display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.other"), QStringLiteral("Other display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.other"));

    controller.focusActionWithoutActivation(QStringLiteral("display.current"));
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.current"));
    QVERIFY(!controller.actionModel()->focusedActionEnabled());
    controller.activateFocusedAction();
    QVERIFY(adapter.executedActionIds.isEmpty());

    controller.refresh();
    QCOMPARE(focusedActionId(controller), QStringLiteral("display.current"));
    controller.activateAction(QStringLiteral("display.current"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void DeckControllerTest::textInputRequestTracksOpenSearchFocus()
{
    DeckController controller;
    QVERIFY(!controller.textInputRequested());

    controller.openFromKeyboard();
    QVERIFY(controller.textInputRequested());
    controller.focusCategories();
    QVERIFY(!controller.textInputRequested());
    controller.focusSearch();
    QVERIFY(controller.textInputRequested());
    controller.close();
    QVERIFY(!controller.textInputRequested());
}

void DeckControllerTest::deckPumpDrainsOnlyDeckCompletions()
{
    MutableHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.refresh"), availableState(QStringLiteral("old")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.refresh"), QStringLiteral("Refresh display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    controller.activateFocusedAction();
    QVERIFY(bool(adapter.pendingCompletion));

    bool unrelatedMetaCallRan = false;
    QObject unrelated;
    QMetaObject::invokeMethod(&unrelated, [&unrelatedMetaCallRan] {
        unrelatedMetaCallRan = true;
    }, Qt::QueuedConnection);
    adapter.currentSnapshot.actionStates[QStringLiteral("display.refresh")].value =
        QStringLiteral("new");
    std::thread completionThread([completion = std::move(adapter.pendingCompletion)] {
        completion(ActionResult {true, QStringLiteral("done"), {}, {}});
    });
    completionThread.join();

    QVERIFY(controller.pumpPendingWork());
    QVERIFY(!unrelatedMetaCallRan);
    QCOMPARE(valueForAction(controller, QStringLiteral("display.refresh")),
             QStringLiteral("new"));
}

REGISTER_PERIGEE_TEST(DeckControllerTest);

#include "test_deckcontroller.moc"
