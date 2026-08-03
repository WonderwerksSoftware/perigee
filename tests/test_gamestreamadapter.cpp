#include "test_registry.h"

#include "perigee/actions/actionregistry.h"
#include "perigee/actions/gamestreamadapter.h"
#include "perigee/actions/sessionfacade.h"

#include <QPointer>
#include <QtTest>

#include <functional>
#include <memory>
#include <type_traits>

namespace {

template<typename T, typename = void>
struct HasPublicExecute : std::false_type {
};

template<typename T>
struct HasPublicExecute<T, std::void_t<decltype(&T::execute)>> : std::true_type {
};

static_assert(!HasPublicExecute<HostAdapter>::value);
static_assert(!HasPublicExecute<GameStreamAdapter>::value);

class FakeSession final : public SessionFacade
{
public:
    bool statsOverlayEnabled() const override { return stats; }
    bool mouseCaptureEnabled() const override { return mouse; }
    bool keyboardCaptureEnabled() const override { return keyboard; }
    bool fullscreenEnabled() const override
    {
        const bool observed = fullscreen;
        auto destroy = std::move(destroyAfterFullscreenRead);
        if (destroy) {
            destroy();
        }
        return observed;
    }

    bool setStatsOverlayEnabled(bool enabled) override
    {
        ++statsSetCount;
        stats = ignoreStatsRequest ? stats : enabled;
        return stats;
    }

    bool setMouseCaptureEnabled(bool enabled) override
    {
        ++mouseSetCount;
        mouse = ignoreMouseRequest ? mouse : enabled;
        return mouse;
    }

    bool setKeyboardCaptureEnabled(bool enabled) override
    {
        ++keyboardSetCount;
        keyboard = ignoreKeyboardRequest ? keyboard : enabled;
        return keyboard;
    }

    bool setFullscreenEnabled(bool enabled) override
    {
        ++fullscreenSetCount;
        fullscreen = ignoreFullscreenRequest ? fullscreen : enabled;
        return fullscreen;
    }

    bool requestClientDisconnect() override
    {
        ++disconnectCount;
        return acceptDisconnectRequest;
    }
    bool requestPerigeeQuit() override
    {
        ++quitCount;
        return acceptQuitRequest;
    }

    bool stats = false;
    bool mouse = false;
    bool keyboard = false;
    bool fullscreen = false;
    bool ignoreStatsRequest = false;
    bool ignoreMouseRequest = false;
    bool ignoreKeyboardRequest = false;
    bool ignoreFullscreenRequest = false;
    bool acceptDisconnectRequest = true;
    bool acceptQuitRequest = true;
    mutable std::function<void()> destroyAfterFullscreenRead;
    int statsSetCount = 0;
    int mouseSetCount = 0;
    int keyboardSetCount = 0;
    int fullscreenSetCount = 0;
    int disconnectCount = 0;
    int quitCount = 0;
};

ActionResult execute(GameStreamAdapter& adapter,
                     const QString& id,
                     const QVariantMap& parameters = {})
{
    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);
    ActionResult result;
    int completionCount = 0;
    registry.execute(id, parameters, [&](const ActionResult& completed) {
        result = completed;
        ++completionCount;
    });
    if (completionCount != 1) {
        result = {false, {}, QStringLiteral("wrong_completion_count"), {}};
    }
    return result;
}

QHash<QString, ActionDescriptor> descriptorMap()
{
    QHash<QString, ActionDescriptor> result;
    for (const ActionDescriptor& descriptor : GameStreamAdapter::descriptors()) {
        result.insert(descriptor.id, descriptor);
    }
    return result;
}

QVariantMap enabled(bool value)
{
    return {{QStringLiteral("enabled"), value}};
}

ActionResult executeConfirmed(GameStreamAdapter& adapter, const QString& id)
{
    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);
    ActionResult result;
    int completionCount = 0;
    if (!registry.beginConfirmation(id)) {
        return {false, {}, QStringLiteral("confirmation_not_started"), {}};
    }
    registry.acceptConfirmation(id, {}, [&](const ActionResult& completed) {
        result = completed;
        ++completionCount;
    });
    if (completionCount != 1) {
        return {false, {}, QStringLiteral("wrong_completion_count"), {}};
    }
    return result;
}

}

class GameStreamAdapterTest : public QObject
{
    Q_OBJECT

private slots:
    void exposesExactLocalActionMetadata();
    void snapshotReadsCurrentSessionState();
    void togglesStatisticsWithObservedEvidence();
    void refusesStatisticsSuccessWhenReadbackDisagrees();
    void togglesMouseCaptureWithObservedEvidence();
    void togglesKeyboardCaptureWithObservedEvidence();
    void releasesBothCaptureIntents();
    void refusesReleaseSuccessWhenEitherReadbackDisagrees();
    void togglesFullscreenWithObservedEvidence();
    void disconnectRequiresConfirmationAndDoesNotQuitPerigee();
    void quitRequiresConfirmationAndDoesNotUseDisconnectOperation();
    void forgedConfirmationParameterIsRejected();
    void rejectedDisconnectRequestIsReportedTruthfully();
    void rejectedQuitRequestIsReportedTruthfully();
    void sessionTerminalResultsCarryObservedState_data();
    void sessionTerminalResultsCarryObservedState();
    void terminalEvidenceTracksAuthoritativeToggleState_data();
    void terminalEvidenceTracksAuthoritativeToggleState();
    void failureEvidenceClearsWhenAuthoritativeStateChanges();
    void subsequentExecutionReplacesTerminalBaseline();
    void destructiveConfirmationCopyIsDistinct();
    void absentSessionFailsClosed();
    void facadeIdentityIsQObjectEnforced();
    void destructiveRequestsReturnAcceptance();
    void destroyedSessionFailsClosed();
    void facadeLossAfterEnabledSnapshotPreservesFailureEvidence();
    void unknownActionFailsClosed();
};

void GameStreamAdapterTest::exposesExactLocalActionMetadata()
{
    const auto descriptors = descriptorMap();
    QCOMPARE(descriptors.size(), 7);
    const QSet<QString> expectedIds {
        QStringLiteral("input.mouse-capture"),
        QStringLiteral("input.keyboard-capture"),
        QStringLiteral("input.release-captured"),
        QStringLiteral("stats.overlay"),
        QStringLiteral("window.fullscreen"),
        QStringLiteral("session.disconnect-client"),
        QStringLiteral("session.quit-perigee"),
    };
    QCOMPARE(QSet<QString>(descriptors.keyBegin(), descriptors.keyEnd()), expectedIds);

    QCOMPARE(descriptors.value(QStringLiteral("input.mouse-capture")).category,
             ActionCategory::Input);
    QCOMPARE(descriptors.value(QStringLiteral("input.keyboard-capture")).category,
             ActionCategory::Input);
    QCOMPARE(descriptors.value(QStringLiteral("input.release-captured")).category,
             ActionCategory::Input);
    QCOMPARE(descriptors.value(QStringLiteral("stats.overlay")).category,
             ActionCategory::Stats);
    QCOMPARE(descriptors.value(QStringLiteral("window.fullscreen")).category,
             ActionCategory::Window);
    QCOMPARE(descriptors.value(QStringLiteral("session.disconnect-client")).category,
             ActionCategory::Session);
    QCOMPARE(descriptors.value(QStringLiteral("session.quit-perigee")).category,
             ActionCategory::Session);

    QCOMPARE(descriptors.value(QStringLiteral("session.disconnect-client")).confirmation,
             ConfirmationPolicy::Always);
    QCOMPARE(descriptors.value(QStringLiteral("session.quit-perigee")).confirmation,
             ConfirmationPolicy::Always);
    QCOMPARE(descriptors.value(QStringLiteral("stats.overlay")).confirmation,
             ConfirmationPolicy::Never);
}

void GameStreamAdapterTest::snapshotReadsCurrentSessionState()
{
    FakeSession session;
    session.stats = true;
    session.mouse = false;
    session.keyboard = true;
    session.fullscreen = false;
    GameStreamAdapter adapter(&session);

    const HostSnapshot snapshot = adapter.snapshot();
    QCOMPARE(snapshot.actionStates.size(), 7);
    QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
    QCOMPARE(snapshot.actionStates.value(QStringLiteral("stats.overlay")).value.toBool(), true);
    QCOMPARE(snapshot.actionStates.value(QStringLiteral("input.mouse-capture")).value.toBool(), false);
    QCOMPARE(snapshot.actionStates.value(QStringLiteral("input.keyboard-capture")).value.toBool(), true);
    QCOMPARE(snapshot.actionStates.value(QStringLiteral("window.fullscreen")).value.toBool(), false);
}

void GameStreamAdapterTest::togglesStatisticsWithObservedEvidence()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult on = execute(adapter, QStringLiteral("stats.overlay"), enabled(true));
    QVERIFY(on.ok);
    QCOMPARE(session.statsSetCount, 1);
    QCOMPARE(on.evidence, QStringLiteral("Statistics overlay: on"));

    const ActionResult off = execute(adapter, QStringLiteral("stats.overlay"), enabled(false));
    QVERIFY(off.ok);
    QCOMPARE(session.statsSetCount, 2);
    QCOMPARE(off.evidence, QStringLiteral("Statistics overlay: off"));
}

void GameStreamAdapterTest::refusesStatisticsSuccessWhenReadbackDisagrees()
{
    FakeSession session;
    session.ignoreStatsRequest = true;
    GameStreamAdapter adapter(&session);

    const ActionResult result = execute(adapter, QStringLiteral("stats.overlay"), enabled(true));
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("state_mismatch"));
    QCOMPARE(result.evidence, QString());
}

void GameStreamAdapterTest::togglesMouseCaptureWithObservedEvidence()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult on = execute(adapter, QStringLiteral("input.mouse-capture"), enabled(true));
    QVERIFY(on.ok);
    QCOMPARE(on.evidence, QStringLiteral("Mouse capture: on"));
    const ActionResult off = execute(adapter, QStringLiteral("input.mouse-capture"), enabled(false));
    QVERIFY(off.ok);
    QCOMPARE(off.evidence, QStringLiteral("Mouse capture: off"));
}

void GameStreamAdapterTest::togglesKeyboardCaptureWithObservedEvidence()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult on = execute(adapter, QStringLiteral("input.keyboard-capture"), enabled(true));
    QVERIFY(on.ok);
    QCOMPARE(on.evidence, QStringLiteral("Keyboard capture: on"));
    const ActionResult off = execute(adapter, QStringLiteral("input.keyboard-capture"), enabled(false));
    QVERIFY(off.ok);
    QCOMPARE(off.evidence, QStringLiteral("Keyboard capture: off"));
}

void GameStreamAdapterTest::releasesBothCaptureIntents()
{
    FakeSession session;
    session.mouse = true;
    session.keyboard = true;
    GameStreamAdapter adapter(&session);

    const ActionResult result = execute(adapter, QStringLiteral("input.release-captured"));
    QVERIFY(result.ok);
    QCOMPARE(session.mouseSetCount, 1);
    QCOMPARE(session.keyboardSetCount, 1);
    QVERIFY(!session.mouse);
    QVERIFY(!session.keyboard);
    QCOMPARE(result.evidence, QStringLiteral("Mouse and keyboard capture: released"));
}

void GameStreamAdapterTest::refusesReleaseSuccessWhenEitherReadbackDisagrees()
{
    FakeSession session;
    session.mouse = true;
    session.keyboard = true;
    session.ignoreKeyboardRequest = true;
    GameStreamAdapter adapter(&session);

    const ActionResult result = execute(adapter, QStringLiteral("input.release-captured"));
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("state_mismatch"));
    QCOMPARE(result.evidence, QString());
}

void GameStreamAdapterTest::togglesFullscreenWithObservedEvidence()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult fullscreen = execute(adapter, QStringLiteral("window.fullscreen"), enabled(true));
    QVERIFY(fullscreen.ok);
    QCOMPARE(fullscreen.evidence, QStringLiteral("Window mode: fullscreen"));
    const ActionResult windowed = execute(adapter, QStringLiteral("window.fullscreen"), enabled(false));
    QVERIFY(windowed.ok);
    QCOMPARE(windowed.evidence, QStringLiteral("Window mode: windowed"));
}

void GameStreamAdapterTest::disconnectRequiresConfirmationAndDoesNotQuitPerigee()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult denied = execute(adapter, QStringLiteral("session.disconnect-client"));
    QVERIFY(!denied.ok);
    QCOMPARE(denied.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(session.disconnectCount, 0);
    QCOMPARE(session.quitCount, 0);

    const ActionResult accepted = executeConfirmed(
        adapter, QStringLiteral("session.disconnect-client"));
    QVERIFY(accepted.ok);
    QCOMPARE(session.disconnectCount, 1);
    QCOMPARE(session.quitCount, 0);
    QCOMPARE(accepted.evidence, QStringLiteral("Client disconnect requested"));
}

void GameStreamAdapterTest::quitRequiresConfirmationAndDoesNotUseDisconnectOperation()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult denied = execute(adapter, QStringLiteral("session.quit-perigee"));
    QVERIFY(!denied.ok);
    QCOMPARE(denied.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(session.disconnectCount, 0);
    QCOMPARE(session.quitCount, 0);

    const ActionResult accepted = executeConfirmed(
        adapter, QStringLiteral("session.quit-perigee"));
    QVERIFY(accepted.ok);
    QCOMPARE(session.disconnectCount, 0);
    QCOMPARE(session.quitCount, 1);
    QCOMPARE(accepted.evidence, QStringLiteral("Perigee quit requested"));
}

void GameStreamAdapterTest::forgedConfirmationParameterIsRejected()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);

    const ActionResult result = execute(
        adapter,
        QStringLiteral("session.disconnect-client"),
        {{QStringLiteral("confirmed"), true}});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(session.disconnectCount, 0);
}

void GameStreamAdapterTest::rejectedDisconnectRequestIsReportedTruthfully()
{
    FakeSession session;
    session.acceptDisconnectRequest = false;
    GameStreamAdapter adapter(&session);

    const ActionResult result = executeConfirmed(
        adapter, QStringLiteral("session.disconnect-client"));

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("request_rejected"));
    QCOMPARE(result.evidence, QString());
    QCOMPARE(session.disconnectCount, 1);
}

void GameStreamAdapterTest::rejectedQuitRequestIsReportedTruthfully()
{
    FakeSession session;
    session.acceptQuitRequest = false;
    GameStreamAdapter adapter(&session);

    const ActionResult result = executeConfirmed(
        adapter, QStringLiteral("session.quit-perigee"));

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("request_rejected"));
    QCOMPARE(result.evidence, QString());
    QCOMPARE(session.quitCount, 1);
}

void GameStreamAdapterTest::sessionTerminalResultsCarryObservedState_data()
{
    QTest::addColumn<QString>("actionId");
    QTest::addColumn<bool>("requestAccepted");

    QTest::newRow("disconnect accepted")
        << QStringLiteral("session.disconnect-client") << true;
    QTest::newRow("disconnect rejected")
        << QStringLiteral("session.disconnect-client") << false;
    QTest::newRow("quit accepted")
        << QStringLiteral("session.quit-perigee") << true;
    QTest::newRow("quit rejected")
        << QStringLiteral("session.quit-perigee") << false;
}

void GameStreamAdapterTest::sessionTerminalResultsCarryObservedState()
{
    QFETCH(QString, actionId);
    QFETCH(bool, requestAccepted);
    FakeSession session;
    session.acceptDisconnectRequest = requestAccepted;
    session.acceptQuitRequest = requestAccepted;
    GameStreamAdapter adapter(&session);

    const ActionResult result = executeConfirmed(adapter, actionId);

    QCOMPARE(result.ok, requestAccepted);
    QVERIFY(result.observedState.has_value());
    QVERIFY(result.observedState->enabled);
    QVERIFY(!result.observedState->value.isValid());
    QVERIFY(result.observedState->disabledCode.isEmpty());
    QVERIFY(result.observedState->disabledReason.isEmpty());
}

void GameStreamAdapterTest::terminalEvidenceTracksAuthoritativeToggleState_data()
{
    QTest::addColumn<QString>("actionId");

    QTest::newRow("statistics") << QStringLiteral("stats.overlay");
    QTest::newRow("fullscreen") << QStringLiteral("window.fullscreen");
}

void GameStreamAdapterTest::terminalEvidenceTracksAuthoritativeToggleState()
{
    QFETCH(QString, actionId);
    FakeSession session;
    GameStreamAdapter adapter(&session);
    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);
    ActionResult result;

    registry.execute(
        actionId, enabled(true),
        [&result](const ActionResult& completed) { result = completed; });
    QVERIFY(result.ok);

    const ActionState firstRead = registry.state(actionId);
    QCOMPARE(firstRead.phase, ActionPhase::Succeeded);
    QVERIFY(!firstRead.message.isEmpty());
    const ActionState unchangedRead = registry.state(actionId);
    QCOMPARE(unchangedRead.phase, ActionPhase::Succeeded);
    QCOMPARE(unchangedRead.message, firstRead.message);

    if (actionId == QStringLiteral("stats.overlay")) {
        session.stats = false;
    }
    else {
        session.fullscreen = false;
    }

    const ActionState changed = registry.state(actionId);
    QCOMPARE(changed.value.toBool(), false);
    QCOMPARE(changed.phase, ActionPhase::Idle);
    QVERIFY(changed.message.isEmpty());
}

void GameStreamAdapterTest::failureEvidenceClearsWhenAuthoritativeStateChanges()
{
    FakeSession session;
    session.ignoreStatsRequest = true;
    GameStreamAdapter adapter(&session);
    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);
    ActionResult result;

    registry.execute(
        QStringLiteral("stats.overlay"), enabled(true),
        [&result](const ActionResult& completed) { result = completed; });
    QVERIFY(!result.ok);
    QCOMPARE(registry.state(QStringLiteral("stats.overlay")).phase,
             ActionPhase::Failed);

    session.stats = true;
    const ActionState changed = registry.state(QStringLiteral("stats.overlay"));
    QCOMPARE(changed.value.toBool(), true);
    QCOMPARE(changed.phase, ActionPhase::Idle);
    QVERIFY(changed.message.isEmpty());
}

void GameStreamAdapterTest::subsequentExecutionReplacesTerminalBaseline()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);
    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);

    registry.execute(QStringLiteral("stats.overlay"), enabled(true),
                     [](const ActionResult&) {});
    QCOMPARE(registry.state(QStringLiteral("stats.overlay")).phase,
             ActionPhase::Succeeded);

    registry.execute(QStringLiteral("stats.overlay"), enabled(false),
                     [](const ActionResult&) {});
    const ActionState disabled = registry.state(QStringLiteral("stats.overlay"));
    QCOMPARE(disabled.phase, ActionPhase::Succeeded);
    QCOMPARE(disabled.message, QStringLiteral("Statistics overlay: off"));
    QCOMPARE(disabled.value.toBool(), false);

    session.stats = true;
    const ActionState changed = registry.state(QStringLiteral("stats.overlay"));
    QCOMPARE(changed.phase, ActionPhase::Idle);
    QVERIFY(changed.message.isEmpty());
}

void GameStreamAdapterTest::destructiveConfirmationCopyIsDistinct()
{
    const auto descriptors = descriptorMap();
    QCOMPARE(descriptors.value(QStringLiteral("session.disconnect-client")).confirmationMessage,
             QStringLiteral("Disconnect this client? Perigee will stay open, and the host session will continue."));
    QCOMPARE(descriptors.value(QStringLiteral("session.quit-perigee")).confirmationMessage,
             QStringLiteral("Quit Perigee? The client will close, and the host session will continue."));
    QVERIFY(descriptors.value(QStringLiteral("session.disconnect-client")).confirmationMessage !=
            descriptors.value(QStringLiteral("session.quit-perigee")).confirmationMessage);
}

void GameStreamAdapterTest::absentSessionFailsClosed()
{
    GameStreamAdapter adapter(nullptr);
    const HostSnapshot snapshot = adapter.snapshot();
    QVERIFY(!snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
    QCOMPARE(snapshot.actionStates.value(QStringLiteral("stats.overlay")).disabledReason,
             QStringLiteral("The streaming session is unavailable."));

    const ActionResult result = execute(adapter, QStringLiteral("stats.overlay"), enabled(true));
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("session_unavailable"));
}

void GameStreamAdapterTest::facadeIdentityIsQObjectEnforced()
{
    QVERIFY((std::is_base_of_v<QObject, SessionFacade>));
}

void GameStreamAdapterTest::destructiveRequestsReturnAcceptance()
{
    QVERIFY((std::is_same_v<
             decltype(std::declval<SessionFacade&>().requestClientDisconnect()),
             bool>));
    QVERIFY((std::is_same_v<
             decltype(std::declval<SessionFacade&>().requestPerigeeQuit()),
             bool>));
}

void GameStreamAdapterTest::destroyedSessionFailsClosed()
{
    auto session = std::make_unique<FakeSession>();
    GameStreamAdapter adapter(session.get());
    session.reset();

    const HostSnapshot snapshot = adapter.snapshot();
    QVERIFY(!snapshot.actionStates.value(QStringLiteral("input.mouse-capture")).enabled);
    const ActionResult result = execute(adapter, QStringLiteral("input.mouse-capture"), enabled(true));
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("session_unavailable"));
}

void GameStreamAdapterTest::facadeLossAfterEnabledSnapshotPreservesFailureEvidence()
{
    auto session = std::make_unique<FakeSession>();
    GameStreamAdapter adapter(session.get());
    ActionRegistry registry(GameStreamAdapter::descriptors(), adapter);
    session->destroyAfterFullscreenRead = [&session] { session.reset(); };

    ActionResult result;
    int completionCount = 0;
    registry.execute(
        QStringLiteral("stats.overlay"), enabled(true),
        [&](const ActionResult& completed) {
            result = completed;
            ++completionCount;
        });

    QCOMPARE(completionCount, 1);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("session_unavailable"));
    QVERIFY(result.observedState.has_value());

    const ActionState failed = registry.state(QStringLiteral("stats.overlay"));
    QCOMPARE(failed.phase, ActionPhase::Failed);
    QCOMPARE(failed.message,
             QStringLiteral("The streaming session is unavailable."));
    QVERIFY(!failed.enabled);
    QCOMPARE(failed.disabledCode, QStringLiteral("session_unavailable"));
    QCOMPARE(failed.disabledReason,
             QStringLiteral("The streaming session is unavailable."));

    const ActionState unchanged = registry.state(QStringLiteral("stats.overlay"));
    QCOMPARE(unchanged.phase, ActionPhase::Failed);
    QCOMPARE(unchanged.message, failed.message);
    QCOMPARE(unchanged.disabledCode, failed.disabledCode);
    QCOMPARE(unchanged.disabledReason, failed.disabledReason);
}

void GameStreamAdapterTest::unknownActionFailsClosed()
{
    FakeSession session;
    GameStreamAdapter adapter(&session);
    const ActionResult result = execute(adapter, QStringLiteral("host.command.guess"));
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("action_unavailable"));
    QCOMPARE(session.statsSetCount, 0);
    QCOMPARE(session.disconnectCount, 0);
}

REGISTER_PERIGEE_TEST(GameStreamAdapterTest);

#include "test_gamestreamadapter.moc"
