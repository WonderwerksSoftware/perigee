#include "test_registry.h"

#include "perigee/actions/actionregistry.h"

#include <QtTest>

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<ActionRegistry>);
static_assert(!std::is_copy_assignable_v<ActionRegistry>);
static_assert(!std::is_move_constructible_v<ActionRegistry>);
static_assert(!std::is_move_assignable_v<ActionRegistry>);
static_assert(!std::is_copy_constructible_v<ActionInvocation>);
static_assert(!std::is_copy_assignable_v<ActionInvocation>);
static_assert(std::is_move_constructible_v<ActionInvocation>);
static_assert(std::is_move_assignable_v<ActionInvocation>);
static_assert(std::is_same_v<decltype(ActionResult {}.observedState),
                             std::optional<ActionState>>);

namespace {

ActionDescriptor descriptor(const QString& id,
                            const QString& label,
                            ActionCategory category,
                            const QStringList& aliases = {},
                            const QString& resourceKey = {},
                            const QString& requiredCapability = {},
                            quint32 requiredPermissions = 0,
                            ConfirmationPolicy confirmation = ConfirmationPolicy::Never)
{
    return { id, label, category, aliases, resourceKey, requiredCapability,
             requiredPermissions, confirmation, {} };
}

QStringList idsOf(const QVector<ActionDescriptor>& descriptors)
{
    QStringList ids;
    for (const ActionDescriptor& action : descriptors) {
        ids.push_back(action.id);
    }
    return ids;
}

ActionState availableState(const QVariant& value = {}, bool disruptive = false)
{
    ActionState state;
    state.enabled = true;
    state.value = value;
    state.disruptive = disruptive;
    return state;
}

}

class FakeHostAdapter : public HostAdapter
{
public:
    HostSnapshot currentSnapshot;
    int executeCallCount = 0;
    QStringList executedActionIds;
    QVector<QVariantMap> executedParameters;
    QVector<Completion> pendingCompletions;
    std::optional<ActionResult> synchronousResult;

    HostSnapshot snapshot() override
    {
        return currentSnapshot;
    }

    void execute(const QString& actionId,
                 QVariantMap parameters,
                 Completion completion) override
    {
        ++executeCallCount;
        executedActionIds.push_back(actionId);
        executedParameters.push_back(std::move(parameters));
        if (synchronousResult.has_value()) {
            completion(*synchronousResult);
            return;
        }
        pendingCompletions.push_back(std::move(completion));
    }

    void cancel(const QString&) override
    {
    }

    void completeNext(const ActionResult& result)
    {
        Completion completion = pendingCompletions.takeFirst();
        completion(result);
    }
};

class ActionRegistryTest : public QObject
{
    Q_OBJECT

private slots:
    void filtersActionsByCategoryInRegistrationOrder();
    void ranksSearchMatchesAndPreservesRegistrationOrderForTies();
    void matchesDisplayAliasesCaseInsensitively_data();
    void matchesDisplayAliasesCaseInsensitively();
    void disablesActionWhenCapabilityIsNotAdvertised();
    void enablesActionAndPreservesReadbackWhenCapabilityIsAdvertised();
    void disablesActionWhenAnyRequiredPermissionIsMissing();
    void enablesActionWhenAllRequiredPermissionsAreGranted();
    void preservesAdapterDisabledReason();
    void resolvesConfirmationPolicyForInvocationRisk();
    void directExecuteCannotBypassKnownDisruptiveAction();
    void riskChangeAfterBeginConsumesConfirmation();
    void directCallerCannotForgeConfirmationWithParameters();
    void confirmationGrantIsActionBoundOneUseAndStateChecked();
    void failedBeginInvalidatesPendingConfirmation_data();
    void failedBeginInvalidatesPendingConfirmation();
    void successfulBeginReplacesPendingConfirmation();
    void executingAnotherActionInvalidatesPendingConfirmation();
    void rechecksPreconditionsAtExecutionTime();
    void terminalEvidenceClearsWhenAvailabilityChanges();
    void limitsInFlightActionsPerResourceAndTracksCompletion();
    void allowsConcurrentActionsWithoutAResourceKey();
    void ignoresAdapterCompletionAfterRegistryDestruction();
    void handlesSynchronousAdapterCompletion();
    void ignoresDuplicateAdapterCompletion();
    void callerCompletionCanDestroyRegistry();
};

void ActionRegistryTest::filtersActionsByCategoryInRegistrationOrder()
{
    FakeHostAdapter adapter;
    const QVector<ActionDescriptor> descriptors {
        descriptor(QStringLiteral("display.previous"), QStringLiteral("Previous display"), ActionCategory::Display),
        descriptor(QStringLiteral("stats.toggle"), QStringLiteral("Toggle statistics"), ActionCategory::Stats),
        descriptor(QStringLiteral("display.next"), QStringLiteral("Next display"), ActionCategory::Display),
    };
    ActionRegistry registry(descriptors, adapter);

    const QVector<ActionDescriptor> displayActions = registry.actions(ActionCategory::Display);

    QCOMPARE(displayActions.size(), 2);
    QCOMPARE(displayActions.at(0).id, QStringLiteral("display.previous"));
    QCOMPARE(displayActions.at(1).id, QStringLiteral("display.next"));
}

void ActionRegistryTest::ranksSearchMatchesAndPreservesRegistrationOrderForTies()
{
    FakeHostAdapter adapter;
    const QVector<ActionDescriptor> descriptors {
        descriptor(QStringLiteral("category"), QStringLiteral("Choose target"), ActionCategory::Display),
        descriptor(QStringLiteral("alias.substring"), QStringLiteral("Choose target"), ActionCategory::Session,
                   { QStringLiteral("change display now") }),
        descriptor(QStringLiteral("label.prefix.first"), QStringLiteral("Display selector"), ActionCategory::Window),
        descriptor(QStringLiteral("alias.prefix"), QStringLiteral("Choose target"), ActionCategory::Session,
                   { QStringLiteral("display switch") }),
        descriptor(QStringLiteral("label.substring"), QStringLiteral("Select display target"), ActionCategory::Window),
        descriptor(QStringLiteral("label.prefix.second"), QStringLiteral("Display options"), ActionCategory::Input),
    };
    ActionRegistry registry(descriptors, adapter);

    QCOMPARE(idsOf(registry.search(QStringLiteral("display"))),
             QStringList({ QStringLiteral("label.prefix.first"),
                           QStringLiteral("label.prefix.second"),
                           QStringLiteral("label.substring"),
                           QStringLiteral("alias.prefix"),
                           QStringLiteral("alias.substring"),
                           QStringLiteral("category") }));
}

void ActionRegistryTest::matchesDisplayAliasesCaseInsensitively_data()
{
    QTest::addColumn<QString>("query");

    QTest::newRow("monitor") << QStringLiteral("MoNiToR");
    QTest::newRow("screen") << QStringLiteral("ScReEn");
    QTest::newRow("display") << QStringLiteral("DiSpLaY");
}

void ActionRegistryTest::matchesDisplayAliasesCaseInsensitively()
{
    QFETCH(QString, query);
    FakeHostAdapter adapter;
    const QVector<ActionDescriptor> descriptors {
        descriptor(QStringLiteral("target.select"), QStringLiteral("Choose target"), ActionCategory::Window,
                   { QStringLiteral("monitor"), QStringLiteral("screen"), QStringLiteral("display") }),
    };
    ActionRegistry registry(descriptors, adapter);

    QCOMPARE(idsOf(registry.search(query)), QStringList({ QStringLiteral("target.select") }));
}

void ActionRegistryTest::disablesActionWhenCapabilityIsNotAdvertised()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"), availableState());
    ActionRegistry registry({ descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                                         ActionCategory::Display, {}, {}, QStringLiteral("display.select")) },
                            adapter);

    const ActionState state = registry.state(QStringLiteral("display.select"));

    QVERIFY(!state.enabled);
    QCOMPARE(state.disabledReason,
             QStringLiteral("The connected host does not advertise this capability."));
}

void ActionRegistryTest::enablesActionAndPreservesReadbackWhenCapabilityIsAdvertised()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.advertisedCapabilities.insert(QStringLiteral("display.select"));
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"),
                                                availableState(QStringLiteral("Desk monitor")));
    ActionRegistry registry({ descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                                         ActionCategory::Display, {}, {}, QStringLiteral("display.select")) },
                            adapter);

    const ActionState state = registry.state(QStringLiteral("display.select"));

    QVERIFY(state.enabled);
    QCOMPARE(state.value.toString(), QStringLiteral("Desk monitor"));
    QVERIFY(state.disabledReason.isEmpty());
}

void ActionRegistryTest::disablesActionWhenAnyRequiredPermissionIsMissing()
{
    constexpr quint32 SelectDisplay = 0x01;
    constexpr quint32 ManageSession = 0x02;
    FakeHostAdapter adapter;
    adapter.currentSnapshot.grantedPermissions = SelectDisplay;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("session.end"), availableState());
    ActionRegistry registry({ descriptor(QStringLiteral("session.end"), QStringLiteral("End host session"),
                                         ActionCategory::Session, {}, {}, {},
                                         SelectDisplay | ManageSession) },
                            adapter);

    const ActionState state = registry.state(QStringLiteral("session.end"));

    QVERIFY(!state.enabled);
    QCOMPARE(state.disabledReason, QStringLiteral("This paired client lacks permission."));
}

void ActionRegistryTest::enablesActionWhenAllRequiredPermissionsAreGranted()
{
    constexpr quint32 SelectDisplay = 0x01;
    constexpr quint32 ManageSession = 0x02;
    FakeHostAdapter adapter;
    adapter.currentSnapshot.grantedPermissions = SelectDisplay | ManageSession;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("session.end"), availableState());
    ActionRegistry registry({ descriptor(QStringLiteral("session.end"), QStringLiteral("End host session"),
                                         ActionCategory::Session, {}, {}, {},
                                         SelectDisplay | ManageSession) },
                            adapter);

    QVERIFY(registry.state(QStringLiteral("session.end")).enabled);
}

void ActionRegistryTest::preservesAdapterDisabledReason()
{
    FakeHostAdapter adapter;
    ActionState transitioning;
    transitioning.disabledReason = QStringLiteral("The current session is transitioning.");
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"), transitioning);
    ActionRegistry registry({ descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                                         ActionCategory::Display) },
                            adapter);

    const ActionState state = registry.state(QStringLiteral("display.select"));

    QVERIFY(!state.enabled);
    QCOMPARE(state.disabledReason, QStringLiteral("The current session is transitioning."));
}

void ActionRegistryTest::resolvesConfirmationPolicyForInvocationRisk()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("stats.toggle"), availableState({}, true));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.end"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("command.run"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("stats.toggle"), QStringLiteral("Toggle statistics"),
                   ActionCategory::Stats, {}, {}, {}, 0, ConfirmationPolicy::Never),
        descriptor(QStringLiteral("session.end"), QStringLiteral("End host session"),
                   ActionCategory::Session, {}, {}, {}, 0, ConfirmationPolicy::Always),
        descriptor(QStringLiteral("command.run"), QStringLiteral("Run named command"),
                   ActionCategory::Session, {}, {}, {}, 0, ConfirmationPolicy::WhenDisruptive),
    }, adapter);

    QVERIFY(!registry.requiresConfirmation(QStringLiteral("stats.toggle")));
    QVERIFY(registry.requiresConfirmation(QStringLiteral("session.end")));
    QVERIFY(!registry.requiresConfirmation(QStringLiteral("command.run")));
    adapter.currentSnapshot.actionStates[QStringLiteral("command.run")].disruptive = true;
    QVERIFY(registry.requiresConfirmation(QStringLiteral("command.run")));
}

void ActionRegistryTest::directExecuteCannotBypassKnownDisruptiveAction()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("command.run"), availableState({}, true));
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("command accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("command.run"), QStringLiteral("Run command"),
                   ActionCategory::Session, {}, {}, {}, 0,
                   ConfirmationPolicy::WhenDisruptive),
    }, adapter);
    ActionResult result;

    QVERIFY(registry.requiresConfirmation(QStringLiteral("command.run")));
    registry.execute(
        QStringLiteral("command.run"), {},
        [&result](const ActionResult& completed) { result = completed; });

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void ActionRegistryTest::riskChangeAfterBeginConsumesConfirmation()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("command.run"), availableState(QStringLiteral("safe"), true));
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("command accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("command.run"), QStringLiteral("Run command"),
                   ActionCategory::Session, {}, {}, {}, 0,
                   ConfirmationPolicy::WhenDisruptive),
    }, adapter);
    ActionResult result;

    QVERIFY(registry.beginConfirmation(QStringLiteral("command.run")));
    adapter.currentSnapshot.actionStates[QStringLiteral("command.run")].disruptive = false;
    registry.acceptConfirmation(
        QStringLiteral("command.run"), {},
        [&result](const ActionResult& completed) { result = completed; });

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("state_changed"));
    QVERIFY(adapter.executedActionIds.isEmpty());

    registry.acceptConfirmation(
        QStringLiteral("command.run"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void ActionRegistryTest::directCallerCannotForgeConfirmationWithParameters()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.disconnect"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("session.disconnect"),
                   QStringLiteral("Disconnect"),
                   ActionCategory::Session,
                   {}, {}, {}, 0, ConfirmationPolicy::Always),
    }, adapter);
    ActionResult result;

    registry.execute(
        QStringLiteral("session.disconnect"),
        {{QStringLiteral("confirmed"), true}},
        [&result](const ActionResult& completed) { result = completed; });

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void ActionRegistryTest::confirmationGrantIsActionBoundOneUseAndStateChecked()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.disconnect"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.quit"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.change"), availableState());
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("session.disconnect"),
                   QStringLiteral("Disconnect"), ActionCategory::Session,
                   {}, {}, {}, 0, ConfirmationPolicy::Always),
        descriptor(QStringLiteral("session.quit"),
                   QStringLiteral("Quit"), ActionCategory::Session,
                   {}, {}, {}, 0, ConfirmationPolicy::Always),
        descriptor(QStringLiteral("display.change"),
                   QStringLiteral("Change display"), ActionCategory::Display,
                   {}, {}, {}, 0, ConfirmationPolicy::WhenDisruptive),
    }, adapter);
    ActionResult result;

    QVERIFY(registry.beginConfirmation(QStringLiteral("session.disconnect")));
    registry.acceptConfirmation(
        QStringLiteral("session.quit"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());

    registry.acceptConfirmation(
        QStringLiteral("session.disconnect"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());

    QVERIFY(registry.beginConfirmation(QStringLiteral("session.disconnect")));
    registry.acceptConfirmation(
        QStringLiteral("session.disconnect"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QVERIFY(result.ok);
    QCOMPARE(adapter.executedActionIds,
             QStringList({QStringLiteral("session.disconnect")}));

    registry.acceptConfirmation(
        QStringLiteral("session.disconnect"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(adapter.executedActionIds.size(), 1);

    QVERIFY(registry.beginConfirmation(QStringLiteral("session.quit")));
    ActionState disabled;
    disabled.disabledReason = QStringLiteral("Session is no longer available.");
    adapter.currentSnapshot.actionStates[QStringLiteral("session.quit")] = disabled;
    registry.acceptConfirmation(
        QStringLiteral("session.quit"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("state_changed"));
    QCOMPARE(adapter.executedActionIds.size(), 1);
    registry.acceptConfirmation(
        QStringLiteral("session.quit"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));

    QVERIFY(!registry.beginConfirmation(QStringLiteral("display.change")));
    adapter.currentSnapshot.actionStates[QStringLiteral("display.change")].disruptive = true;
    QVERIFY(registry.beginConfirmation(QStringLiteral("display.change")));
    registry.acceptConfirmation(
        QStringLiteral("display.change"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QVERIFY(result.ok);
    QCOMPARE(adapter.executedActionIds.last(), QStringLiteral("display.change"));
}

void ActionRegistryTest::failedBeginInvalidatesPendingConfirmation_data()
{
    QTest::addColumn<QString>("failedActionId");

    QTest::newRow("non-confirming") << QStringLiteral("stats.toggle");
    QTest::newRow("unknown") << QStringLiteral("missing.action");
    QTest::newRow("disabled") << QStringLiteral("session.disabled");
}

void ActionRegistryTest::failedBeginInvalidatesPendingConfirmation()
{
    QFETCH(QString, failedActionId);
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.first"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("stats.toggle"), availableState());
    ActionState disabled;
    disabled.disabledReason = QStringLiteral("Session is unavailable.");
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.disabled"), disabled);
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("session.first"), QStringLiteral("First"),
                   ActionCategory::Session, {}, {}, {}, 0,
                   ConfirmationPolicy::Always),
        descriptor(QStringLiteral("stats.toggle"), QStringLiteral("Statistics"),
                   ActionCategory::Stats),
        descriptor(QStringLiteral("session.disabled"), QStringLiteral("Disabled"),
                   ActionCategory::Session, {}, {}, {}, 0,
                   ConfirmationPolicy::Always),
    }, adapter);
    ActionResult result;

    QVERIFY(registry.beginConfirmation(QStringLiteral("session.first")));
    QVERIFY(!registry.beginConfirmation(failedActionId));
    registry.acceptConfirmation(
        QStringLiteral("session.first"), {},
        [&result](const ActionResult& completed) { result = completed; });

    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(adapter.executedActionIds.isEmpty());
}

void ActionRegistryTest::successfulBeginReplacesPendingConfirmation()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.first"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.second"), availableState());
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("session.first"), QStringLiteral("First"),
                   ActionCategory::Session, {}, {}, {}, 0,
                   ConfirmationPolicy::Always),
        descriptor(QStringLiteral("session.second"), QStringLiteral("Second"),
                   ActionCategory::Session, {}, {}, {}, 0,
                   ConfirmationPolicy::Always),
    }, adapter);
    ActionResult result;

    QVERIFY(registry.beginConfirmation(QStringLiteral("session.first")));
    QVERIFY(registry.beginConfirmation(QStringLiteral("session.second")));
    registry.acceptConfirmation(
        QStringLiteral("session.second"), {},
        [&result](const ActionResult& completed) { result = completed; });

    QVERIFY(result.ok);
    QCOMPARE(adapter.executedActionIds,
             QStringList({QStringLiteral("session.second")}));

    registry.acceptConfirmation(
        QStringLiteral("session.first"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(adapter.executedActionIds.size(), 1);
}

void ActionRegistryTest::executingAnotherActionInvalidatesPendingConfirmation()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.disconnect"), availableState());
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("stats.toggle"), availableState());
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("accepted"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("session.disconnect"),
                   QStringLiteral("Disconnect"), ActionCategory::Session,
                   {}, {}, {}, 0, ConfirmationPolicy::Always),
        descriptor(QStringLiteral("stats.toggle"),
                   QStringLiteral("Statistics"), ActionCategory::Stats),
    }, adapter);
    ActionResult result;

    QVERIFY(registry.beginConfirmation(QStringLiteral("session.disconnect")));
    registry.execute(
        QStringLiteral("stats.toggle"), {},
        [&result](const ActionResult& completed) { result = completed; });
    QVERIFY(result.ok);
    registry.acceptConfirmation(
        QStringLiteral("session.disconnect"), {},
        [&result](const ActionResult& completed) { result = completed; });

    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(adapter.executedActionIds,
             QStringList({QStringLiteral("stats.toggle")}));
}

void ActionRegistryTest::rechecksPreconditionsAtExecutionTime()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.advertisedCapabilities.insert(QStringLiteral("display.select"));
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"), availableState());
    ActionRegistry registry({ descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                                         ActionCategory::Display, {}, QStringLiteral("display"),
                                         QStringLiteral("display.select")) },
                            adapter);
    QVERIFY(registry.state(QStringLiteral("display.select")).enabled);
    adapter.currentSnapshot.advertisedCapabilities.clear();
    std::optional<ActionResult> completedResult;

    registry.execute(QStringLiteral("display.select"),
                     { { QStringLiteral("target"), QStringLiteral("desk") } },
                     [&completedResult](const ActionResult& result) {
        completedResult = result;
    });

    QCOMPARE(adapter.executeCallCount, 0);
    QVERIFY(completedResult.has_value());
    QVERIFY(!completedResult->ok);
    QCOMPARE(completedResult->errorCode, QStringLiteral("capability_unavailable"));
    QCOMPARE(completedResult->userMessage,
             QStringLiteral("The connected host does not advertise this capability."));
}

void ActionRegistryTest::terminalEvidenceClearsWhenAvailabilityChanges()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.advertisedCapabilities.insert(
        QStringLiteral("display.select"));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), availableState(QStringLiteral("desk")));
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("Display selected"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                   ActionCategory::Display, {}, {}, QStringLiteral("display.select")),
    }, adapter);

    registry.execute(QStringLiteral("display.select"), {}, [](const ActionResult&) {});
    const ActionState completed = registry.state(QStringLiteral("display.select"));
    QCOMPARE(completed.phase, ActionPhase::Succeeded);
    QCOMPARE(completed.message, QStringLiteral("Display selected"));

    adapter.currentSnapshot.advertisedCapabilities.clear();
    const ActionState unavailable = registry.state(QStringLiteral("display.select"));
    QVERIFY(!unavailable.enabled);
    QCOMPARE(unavailable.disabledCode, QStringLiteral("capability_unavailable"));
    QCOMPARE(unavailable.phase, ActionPhase::Idle);
    QVERIFY(unavailable.message.isEmpty());
}

void ActionRegistryTest::limitsInFlightActionsPerResourceAndTracksCompletion()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.first"), availableState());
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.second"), availableState());
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("stats.toggle"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("display.first"), QStringLiteral("First display"),
                   ActionCategory::Display, {}, QStringLiteral("display")),
        descriptor(QStringLiteral("display.second"), QStringLiteral("Second display"),
                   ActionCategory::Display, {}, QStringLiteral("display")),
        descriptor(QStringLiteral("stats.toggle"), QStringLiteral("Toggle statistics"),
                   ActionCategory::Stats, {}, QStringLiteral("stats")),
    }, adapter);
    std::optional<ActionResult> firstResult;
    std::optional<ActionResult> sameActionBusyResult;
    std::optional<ActionResult> busyResult;
    std::optional<ActionResult> statsResult;

    registry.execute(QStringLiteral("display.first"),
                     { { QStringLiteral("target"), QStringLiteral("first") } },
                     [&firstResult](const ActionResult& result) { firstResult = result; });
    QCOMPARE(adapter.executeCallCount, 1);
    QCOMPARE(adapter.executedActionIds,
             QStringList({ QStringLiteral("display.first") }));
    QCOMPARE(adapter.executedParameters.first().value(QStringLiteral("target")).toString(),
             QStringLiteral("first"));
    QCOMPARE(registry.state(QStringLiteral("display.first")).phase, ActionPhase::Working);

    registry.execute(QStringLiteral("display.first"), {},
                     [&sameActionBusyResult](const ActionResult& result) {
        sameActionBusyResult = result;
    });
    QCOMPARE(adapter.executeCallCount, 1);
    QVERIFY(sameActionBusyResult.has_value());
    QCOMPARE(sameActionBusyResult->errorCode, QStringLiteral("resource_busy"));
    QCOMPARE(sameActionBusyResult->userMessage,
             QStringLiteral("This action is already in progress."));

    registry.execute(QStringLiteral("display.second"), {},
                     [&busyResult](const ActionResult& result) { busyResult = result; });
    QCOMPARE(adapter.executeCallCount, 1);
    QVERIFY(busyResult.has_value());
    QVERIFY(!busyResult->ok);
    QCOMPARE(busyResult->errorCode, QStringLiteral("resource_busy"));
    QCOMPARE(busyResult->userMessage,
             QStringLiteral("Another action for this resource is already in progress."));
    QVERIFY(!registry.state(QStringLiteral("display.second")).enabled);

    registry.execute(QStringLiteral("stats.toggle"), {},
                     [&statsResult](const ActionResult& result) { statsResult = result; });
    QCOMPARE(adapter.executeCallCount, 2);
    QCOMPARE(adapter.executedActionIds.last(), QStringLiteral("stats.toggle"));

    adapter.completeNext({ true, QStringLiteral("Display first confirmed"), {}, {} });
    QVERIFY(firstResult.has_value());
    QVERIFY(firstResult->ok);
    QCOMPARE(registry.state(QStringLiteral("display.first")).phase, ActionPhase::Succeeded);
    QCOMPARE(registry.state(QStringLiteral("display.first")).message,
             QStringLiteral("Display first confirmed"));

    registry.execute(QStringLiteral("display.second"), {}, [](const ActionResult&) {});
    QCOMPARE(adapter.executeCallCount, 3);
    QCOMPARE(adapter.executedActionIds.last(), QStringLiteral("display.second"));

    adapter.completeNext({ false, {}, QStringLiteral("network"),
                           QStringLiteral("Host became unreachable.") });
    QVERIFY(statsResult.has_value());
    QVERIFY(!statsResult->ok);
    QCOMPARE(registry.state(QStringLiteral("stats.toggle")).phase, ActionPhase::Failed);
    QCOMPARE(registry.state(QStringLiteral("stats.toggle")).message,
             QStringLiteral("Host became unreachable."));
    adapter.completeNext({ true, QStringLiteral("Display second confirmed"), {}, {} });
}

void ActionRegistryTest::allowsConcurrentActionsWithoutAResourceKey()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("window.fullscreen"), availableState());
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("input.release"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("window.fullscreen"), QStringLiteral("Toggle fullscreen"),
                   ActionCategory::Window),
        descriptor(QStringLiteral("input.release"), QStringLiteral("Release captured input"),
                   ActionCategory::Input),
    }, adapter);

    registry.execute(QStringLiteral("window.fullscreen"), {}, [](const ActionResult&) {});
    registry.execute(QStringLiteral("input.release"), {}, [](const ActionResult&) {});

    QCOMPARE(adapter.executeCallCount, 2);
}

void ActionRegistryTest::ignoresAdapterCompletionAfterRegistryDestruction()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"), availableState());
    bool callerCompletionInvoked = false;
    {
        auto registry = std::make_unique<ActionRegistry>(
                QVector<ActionDescriptor> {
                    descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                               ActionCategory::Display, {}, QStringLiteral("display")),
                },
                adapter);
        registry->execute(QStringLiteral("display.select"), {},
                          [&callerCompletionInvoked](const ActionResult&) {
            callerCompletionInvoked = true;
        });
        QCOMPARE(adapter.pendingCompletions.size(), 1);
    }

    adapter.completeNext({ true, QStringLiteral("Display confirmed"), {}, {} });

    QVERIFY(!callerCompletionInvoked);
}

void ActionRegistryTest::handlesSynchronousAdapterCompletion()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"), availableState());
    adapter.synchronousResult = ActionResult {
        true, QStringLiteral("Display confirmed synchronously"), {}, {}
    };
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                   ActionCategory::Display, {}, QStringLiteral("display")),
    }, adapter);
    std::optional<ActionResult> completedResult;

    registry.execute(QStringLiteral("display.select"), {},
                     [&completedResult](const ActionResult& result) {
        completedResult = result;
    });

    QVERIFY(completedResult.has_value());
    QVERIFY(completedResult->ok);
    QCOMPARE(registry.state(QStringLiteral("display.select")).phase,
             ActionPhase::Succeeded);
    QCOMPARE(registry.state(QStringLiteral("display.select")).message,
             QStringLiteral("Display confirmed synchronously"));
    registry.execute(QStringLiteral("display.select"), {}, [](const ActionResult&) {});
    QCOMPARE(adapter.executeCallCount, 2);
}

void ActionRegistryTest::ignoresDuplicateAdapterCompletion()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("stats.toggle"), availableState());
    ActionRegistry registry({
        descriptor(QStringLiteral("stats.toggle"), QStringLiteral("Toggle statistics"),
                   ActionCategory::Stats, {}, QStringLiteral("stats")),
    }, adapter);
    int callerCompletionCount = 0;
    registry.execute(QStringLiteral("stats.toggle"), {},
                     [&callerCompletionCount](const ActionResult&) {
        ++callerCompletionCount;
    });
    HostAdapter::Completion adapterCompletion = adapter.pendingCompletions.takeFirst();

    adapterCompletion({ true, QStringLiteral("Statistics enabled"), {}, {} });
    adapterCompletion({ false, {}, QStringLiteral("late_failure"),
                        QStringLiteral("Late duplicate failure") });

    QCOMPARE(callerCompletionCount, 1);
    QCOMPARE(registry.state(QStringLiteral("stats.toggle")).phase,
             ActionPhase::Succeeded);
    QCOMPARE(registry.state(QStringLiteral("stats.toggle")).message,
             QStringLiteral("Statistics enabled"));
}

void ActionRegistryTest::callerCompletionCanDestroyRegistry()
{
    FakeHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(QStringLiteral("display.select"), availableState());
    auto registry = std::make_unique<ActionRegistry>(
            QVector<ActionDescriptor> {
                descriptor(QStringLiteral("display.select"), QStringLiteral("Select display"),
                           ActionCategory::Display, {}, QStringLiteral("display")),
            },
            adapter);
    int callerCompletionCount = 0;
    ActionPhase phaseObservedByCaller = ActionPhase::Idle;
    registry->execute(QStringLiteral("display.select"), {},
                      [&registry, &callerCompletionCount,
                       &phaseObservedByCaller](const ActionResult&) {
        ++callerCompletionCount;
        phaseObservedByCaller = registry->state(QStringLiteral("display.select")).phase;
        registry.reset();
    });
    HostAdapter::Completion adapterCompletion = adapter.pendingCompletions.takeFirst();

    adapterCompletion({ true, QStringLiteral("Display confirmed"), {}, {} });
    adapterCompletion({ false, {}, QStringLiteral("duplicate"),
                        QStringLiteral("Duplicate completion") });

    QCOMPARE(callerCompletionCount, 1);
    QCOMPARE(phaseObservedByCaller, ActionPhase::Succeeded);
    QVERIFY(!registry);
}

REGISTER_PERIGEE_TEST(ActionRegistryTest);

#include "test_actionregistry.moc"
