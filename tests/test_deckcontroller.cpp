#include "test_registry.h"

#include "perigee/actions/actionregistry.h"
#include "perigee/deck/actionlistmodel.h"
#include "perigee/deck/deckcontroller.h"

#include <QAbstractItemModel>
#include <QtTest>

#include <optional>
#include <utility>

namespace {

ActionDescriptor descriptor(const QString& id,
                            const QString& label,
                            ActionCategory category,
                            ConfirmationPolicy confirmation = ConfirmationPolicy::Never)
{
    return { id, label, category, {}, {}, {}, 0, confirmation };
}

ActionState availableState(const QString& value = {})
{
    ActionState state;
    state.enabled = true;
    state.value = value;
    return state;
}

class MutableHostAdapter final : public HostAdapter
{
public:
    HostSnapshot currentSnapshot;
    QStringList executedActionIds;
    std::optional<ActionResult> synchronousResult;

    HostSnapshot snapshot() override
    {
        return currentSnapshot;
    }

    void execute(const QString& actionId,
                 const QVariantMap&,
                 Completion completion) override
    {
        executedActionIds.push_back(actionId);
        if (synchronousResult.has_value()) {
            completion(*synchronousResult);
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

}

class DeckControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void keyboardOpenFocusesSearchAndShowsDisplayActions();
    void categoryCyclingMakesActionsReachableWithoutSearchText();
    void refreshPreservesFocusedActionAndUpdatesItsState();
    void backUnwindsConfirmationActionsSearchAndDeck();
    void confirmationCanBeCancelledOrAccepted();
    void disabledFocusedActionFallsBackToAnEnabledPeer();
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
                   ActionCategory::Session, ConfirmationPolicy::Always),
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
                   ActionCategory::Session, ConfirmationPolicy::Always),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.setSearchText(QStringLiteral("session"));
    controller.focusActions();

    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());
    QCOMPARE(controller.confirmationActionLabel(), QStringLiteral("End host session"));
    QVERIFY(adapter.executedActionIds.isEmpty());

    controller.cancelConfirmation();
    QVERIFY(!controller.confirmationVisible());
    QVERIFY(adapter.executedActionIds.isEmpty());

    controller.activateFocusedAction();
    controller.acceptConfirmation();
    QCOMPARE(adapter.executedActionIds,
             QStringList({ QStringLiteral("session.end") }));
    QVERIFY(!controller.confirmationVisible());
    QCOMPARE(valueForAction(controller, QStringLiteral("session.end")), QString());
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

REGISTER_PERIGEE_TEST(DeckControllerTest);

#include "test_deckcontroller.moc"
