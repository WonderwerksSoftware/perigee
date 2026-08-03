#include "test_registry.h"

#include "perigee/actions/gamestreamadapter.h"
#include "perigee/actions/sessionfacade.h"

#include <QPointer>
#include <QtTest>

#include <memory>

namespace {

class FakeSession final : public QObject, public SessionFacade
{
public:
    QObject* lifetimeAuthority() override { return this; }

    bool statsOverlayEnabled() const override { return stats; }
    bool mouseCaptureEnabled() const override { return mouse; }
    bool keyboardCaptureEnabled() const override { return keyboard; }
    bool fullscreenEnabled() const override { return fullscreen; }

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

    void requestClientDisconnect() override { ++disconnectCount; }
    void requestPerigeeQuit() override { ++quitCount; }

    bool stats = false;
    bool mouse = false;
    bool keyboard = false;
    bool fullscreen = false;
    bool ignoreStatsRequest = false;
    bool ignoreMouseRequest = false;
    bool ignoreKeyboardRequest = false;
    bool ignoreFullscreenRequest = false;
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
    ActionResult result;
    int completionCount = 0;
    adapter.execute(id, parameters, [&](const ActionResult& completed) {
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

QVariantMap confirmed()
{
    return {{QStringLiteral("confirmed"), true}};
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
    void destructiveConfirmationCopyIsDistinct();
    void absentSessionFailsClosed();
    void destroyedSessionFailsClosed();
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

    const ActionResult accepted = execute(adapter, QStringLiteral("session.disconnect-client"), confirmed());
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

    const ActionResult accepted = execute(adapter, QStringLiteral("session.quit-perigee"), confirmed());
    QVERIFY(accepted.ok);
    QCOMPARE(session.disconnectCount, 0);
    QCOMPARE(session.quitCount, 1);
    QCOMPARE(accepted.evidence, QStringLiteral("Perigee quit requested"));
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
