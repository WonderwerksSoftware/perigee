#include "test_registry.h"

#include "perigee/actions/actionregistry.h"

#include <QtTest>

#include <memory>
#include <optional>
#include <utility>

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
             requiredPermissions, confirmation };
}

QStringList idsOf(const QVector<ActionDescriptor>& descriptors)
{
    QStringList ids;
    for (const ActionDescriptor& action : descriptors) {
        ids.push_back(action.id);
    }
    return ids;
}

ActionState availableState(const QVariant& value = {})
{
    ActionState state;
    state.enabled = true;
    state.value = value;
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

    HostSnapshot snapshot() override
    {
        return currentSnapshot;
    }

    void execute(const QString& actionId,
                 const QVariantMap& parameters,
                 Completion completion) override
    {
        ++executeCallCount;
        executedActionIds.push_back(actionId);
        executedParameters.push_back(parameters);
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
    void rechecksPreconditionsAtExecutionTime();
    void limitsInFlightActionsPerResourceAndTracksCompletion();
    void allowsConcurrentActionsWithoutAResourceKey();
    void ignoresAdapterCompletionAfterRegistryDestruction();
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
    ActionRegistry registry({
        descriptor(QStringLiteral("stats.toggle"), QStringLiteral("Toggle statistics"),
                   ActionCategory::Stats, {}, {}, {}, 0, ConfirmationPolicy::Never),
        descriptor(QStringLiteral("session.end"), QStringLiteral("End host session"),
                   ActionCategory::Session, {}, {}, {}, 0, ConfirmationPolicy::Always),
        descriptor(QStringLiteral("command.run"), QStringLiteral("Run named command"),
                   ActionCategory::Session, {}, {}, {}, 0, ConfirmationPolicy::WhenDisruptive),
    }, adapter);

    QVERIFY(!registry.requiresConfirmation(QStringLiteral("stats.toggle"), true));
    QVERIFY(registry.requiresConfirmation(QStringLiteral("session.end"), false));
    QVERIFY(!registry.requiresConfirmation(QStringLiteral("command.run"), false));
    QVERIFY(registry.requiresConfirmation(QStringLiteral("command.run"), true));
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
    QCOMPARE(completedResult->errorCode, QStringLiteral("state_changed"));
    QCOMPARE(completedResult->userMessage,
             QStringLiteral("The connected host does not advertise this capability."));
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

REGISTER_PERIGEE_TEST(ActionRegistryTest);

#include "test_actionregistry.moc"
