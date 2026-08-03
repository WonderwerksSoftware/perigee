#include "perigee/actions/actionregistry.h"
#include "perigee/actions/gamestreamadapter.h"
#include "perigee/actions/sessionfacade.h"
#include "perigee/polaris/polarisadapter.h"
#include "test_registry.h"

#include <QFile>
#include <QQueue>
#include <QtTest>

#include <algorithm>
#include <memory>

namespace {

constexpr auto CapabilitiesRoute = "/polaris/v1/capabilities";
constexpr auto StatusRoute = "/polaris/v1/session/status";
constexpr auto SettingsRoute = "/polaris/v1/client-settings";
constexpr auto CommandsRoute = "/polaris/v1/commands";

PolarisResponse fixture(const char* name, int status = 200,
                        bool authenticated = true)
{
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
        QStringLiteral("fixtures/polaris/%1").arg(QString::fromLatin1(name)));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Unable to open Polaris fixture: %s", qPrintable(path));
    }
    PolarisResponse response;
    response.httpStatus = status;
    response.body = file.readAll();
    response.json = QJsonDocument::fromJson(response.body);
    response.authenticated = authenticated;
    return response;
}

PolarisResponse failure(const QString& errorCode)
{
    PolarisResponse response;
    response.errorCode = errorCode;
    return response;
}

class FakeSession final : public SessionFacade
{
public:
    bool statsOverlayEnabled() const override { return stats; }
    bool mouseCaptureEnabled() const override { return mouse; }
    bool keyboardCaptureEnabled() const override { return keyboard; }
    bool fullscreenEnabled() const override { return fullscreen; }
    bool setStatsOverlayEnabled(bool value) override { return stats = value; }
    bool setMouseCaptureEnabled(bool value) override { return mouse = value; }
    bool setKeyboardCaptureEnabled(bool value) override { return keyboard = value; }
    bool setFullscreenEnabled(bool value) override { return fullscreen = value; }
    bool requestClientDisconnect() override { ++disconnects; return true; }
    bool requestPerigeeQuit() override { ++quits; return true; }

    bool stats = false;
    bool mouse = false;
    bool keyboard = false;
    bool fullscreen = false;
    int disconnects = 0;
    int quits = 0;
};

struct FakeTransportState
{
    struct Request {
        PolarisTransport::RequestId id = 0;
        QString endpoint;
        PolarisTransport::Completion completion;
    };
    struct Delivery {
        PolarisTransport::RequestId id = 0;
        PolarisResponse response;
    };

    QVector<Request> requests;
    QQueue<Delivery> deliveries;
    QVector<PolarisTransport::RequestId> cancellations;
    PolarisTransport::RequestId nextId = 1;
    int maximumObservedDrain = 0;
    int destroyed = 0;
};

class FakeTransport final : public PolarisTransport
{
public:
    explicit FakeTransport(std::shared_ptr<FakeTransportState> state)
        : m_State(std::move(state))
    {
    }
    ~FakeTransport() override { ++m_State->destroyed; }

    RequestId get(const QString& endpoint, bool, Completion completion,
                  PolarisRequestOptions) override
    {
        const RequestId id = m_State->nextId++;
        m_State->requests.push_back({id, endpoint, std::move(completion)});
        return id;
    }

    RequestId post(const QString& endpoint, const QByteArray&, bool,
                   Completion completion, PolarisRequestOptions) override
    {
        return get(endpoint, true, std::move(completion), {});
    }

    RequestId fetchClipboard(std::optional<qint64>, Completion completion,
                             PolarisRequestOptions) override
    {
        return get(QStringLiteral("/actions/clipboard?type=text"), false,
                   std::move(completion), {});
    }

    RequestId sendClipboard(const QByteArray&, Completion completion,
                            PolarisRequestOptions) override
    {
        return get(QStringLiteral("/actions/clipboard?type=text"), false,
                   std::move(completion), {});
    }

    bool cancel(RequestId id) override
    {
        if (m_State->cancellations.contains(id)) {
            return false;
        }
        m_State->cancellations.push_back(id);
        return true;
    }

    int drainCompletions(int maximum) override
    {
        m_State->maximumObservedDrain = std::max(
            m_State->maximumObservedDrain, maximum);
        int delivered = 0;
        while (delivered < maximum && !m_State->deliveries.isEmpty()) {
            const FakeTransportState::Delivery delivery =
                m_State->deliveries.dequeue();
            const auto it = std::find_if(
                m_State->requests.cbegin(), m_State->requests.cend(),
                [&delivery](const auto& request) { return request.id == delivery.id; });
            if (it != m_State->requests.cend() && it->completion) {
                it->completion(delivery.id, delivery.response);
            }
            ++delivered;
        }
        return delivered;
    }

    qsizetype pendingCompletionCount() const override
    {
        return m_State->deliveries.size();
    }

    QUrl pairedOrigin() const override
    {
        return QUrl(QStringLiteral("https://127.0.0.1:47984"));
    }

private:
    std::shared_ptr<FakeTransportState> m_State;
};

PolarisTransport::RequestId requestId(
    const std::shared_ptr<FakeTransportState>& state,
    const QString& endpoint, int occurrenceFromEnd = 0)
{
    int seen = 0;
    for (auto it = state->requests.crbegin(); it != state->requests.crend(); ++it) {
        if (it->endpoint == endpoint && seen++ == occurrenceFromEnd) {
            return it->id;
        }
    }
    return 0;
}

void queue(const std::shared_ptr<FakeTransportState>& state,
           const QString& endpoint, PolarisResponse response,
           int occurrenceFromEnd = 0)
{
    state->deliveries.enqueue(
        {requestId(state, endpoint, occurrenceFromEnd), std::move(response)});
}

std::unique_ptr<PolarisAdapter> makeAdapter(
    FakeSession& session,
    const std::shared_ptr<FakeTransportState>& transportState,
    std::unique_ptr<GameStreamAdapter>* gameStreamOut = nullptr)
{
    auto gameStream = std::make_unique<GameStreamAdapter>(&session);
    auto adapter = std::make_unique<PolarisAdapter>(
        *gameStream, std::make_unique<FakeTransport>(transportState));
    if (gameStreamOut != nullptr) {
        *gameStreamOut = std::move(gameStream);
    }
    else {
        // Tests that do not request ownership retain it beside the adapter.
        qFatal("makeAdapter requires GameStreamAdapter ownership output");
    }
    return adapter;
}

void queueCompleteGeneration(const std::shared_ptr<FakeTransportState>& state,
                             PolarisResponse capabilities = fixture("capabilities-current.json"),
                             PolarisResponse status = fixture("status-owner.json"),
                             PolarisResponse settings = fixture("client-settings-current.json"),
                             int occurrenceFromEnd = 0)
{
    queue(state, QString::fromLatin1(CapabilitiesRoute), std::move(capabilities),
          occurrenceFromEnd);
    queue(state, QString::fromLatin1(StatusRoute), std::move(status),
          occurrenceFromEnd);
    queue(state, QString::fromLatin1(SettingsRoute), std::move(settings),
          occurrenceFromEnd);
}

void completeGeneration(
    PolarisAdapter& adapter,
    const std::shared_ptr<FakeTransportState>& state,
    PolarisResponse capabilities = fixture("capabilities-current.json"),
    PolarisResponse status = fixture("status-owner.json"),
    PolarisResponse settings = fixture("client-settings-current.json"),
    PolarisResponse commands = fixture("commands-current.json"))
{
    queueCompleteGeneration(state, std::move(capabilities), std::move(status),
                            std::move(settings));
    adapter.pumpCompletions(8);
    if (requestId(state, QString::fromLatin1(CommandsRoute)) != 0) {
        queue(state, QString::fromLatin1(CommandsRoute), std::move(commands));
        adapter.pumpCompletions(8);
    }
}

PolarisResponse replaceJson(PolarisResponse response, QJsonObject object)
{
    response.json = QJsonDocument(std::move(object));
    response.body = response.json.toJson(QJsonDocument::Compact);
    return response;
}

}

class PolarisAdapterTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialDiscoveryUsesExactlyThreeRoutesAndBoundedPumping();
    void completionOrderPublishesOneCoherentGeneration_data();
    void completionOrderPublishesOneCoherentGeneration();
    void authenticatedFallbackCancelsSiblingWorkAndKeepsLocalActions();
    void authenticatedNonContractJsonFallsBackWithoutGuessing();
    void failuresRemainDistinctAndLaterRefreshRecovers_data();
    void failuresRemainDistinctAndLaterRefreshRecovers();
    void staleAndDuplicateCompletionsCannotReplaceNewerGeneration();
    void duplicateCompletionForCurrentGenerationIsIgnored();
    void teardownCancelsOutstandingAndLateCallbacksAreHarmless();
    void noCommandFollowupForFallbackOldOrRejectedEndpoint_data();
    void noCommandFollowupForFallbackOldOrRejectedEndpoint();
    void commandDependencyFailuresDisableOnlyNamedCommands_data();
    void commandDependencyFailuresDisableOnlyNamedCommands();
    void refreshAndTeardownOwnOutstandingCommandFollowup();
    void localGameStreamExecutionIsDelegatedUnchanged();
    void exactDisabledStatesCoverPermissionOwnershipTransitionAndDisplays();
    void partialMalformedDataDisablesOnlyItsDependentFeature();
    void deniedCapabilitiesPrecedeValidDocumentFeatureAbsence();
    void dependencyFailuresRemainExactAndCompositional_data();
    void dependencyFailuresRemainExactAndCompositional();
    void refreshKeepsPreviousSnapshotUntilNewGenerationIsComplete();
};

void PolarisAdapterTest::initialDiscoveryUsesExactlyThreeRoutesAndBoundedPumping()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);

    QVERIFY(adapter->startDiscovery());
    QCOMPARE(state->requests.size(), 3);
    QCOMPARE(state->requests.at(0).endpoint, QString::fromLatin1(CapabilitiesRoute));
    QCOMPARE(state->requests.at(1).endpoint, QString::fromLatin1(StatusRoute));
    QCOMPARE(state->requests.at(2).endpoint, QString::fromLatin1(SettingsRoute));
    QVERIFY(!adapter->startDiscovery());

    queueCompleteGeneration(state);
    QCOMPARE(adapter->pumpCompletions(1), 1);
    QVERIFY(!adapter->discoverySnapshot().complete);
    QCOMPARE(adapter->pumpCompletions(1), 1);
    QVERIFY(!adapter->discoverySnapshot().complete);
    QCOMPARE(adapter->pumpCompletions(1), 1);
    QVERIFY(!adapter->discoverySnapshot().complete);
    QCOMPARE(state->requests.size(), 4);
    QCOMPARE(state->requests.at(3).endpoint,
             QString::fromLatin1(CommandsRoute));
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    QCOMPARE(adapter->pumpCompletions(1), 1);
    QVERIFY(adapter->discoverySnapshot().complete);
    QCOMPARE(adapter->discoverySnapshot().capabilities.commands.size(), 2);
    QCOMPARE(state->maximumObservedDrain, 1);
}

void PolarisAdapterTest::completionOrderPublishesOneCoherentGeneration_data()
{
    QTest::addColumn<QStringList>("order");
    QTest::newRow("cap-status-settings")
        << QStringList{CapabilitiesRoute, StatusRoute, SettingsRoute};
    QTest::newRow("settings-cap-status")
        << QStringList{SettingsRoute, CapabilitiesRoute, StatusRoute};
    QTest::newRow("status-settings-cap")
        << QStringList{StatusRoute, SettingsRoute, CapabilitiesRoute};
    QTest::newRow("settings-status-cap")
        << QStringList{SettingsRoute, StatusRoute, CapabilitiesRoute};
    QTest::newRow("cap-settings-status")
        << QStringList{CapabilitiesRoute, SettingsRoute, StatusRoute};
    QTest::newRow("status-cap-settings")
        << QStringList{StatusRoute, CapabilitiesRoute, SettingsRoute};
}

void PolarisAdapterTest::authenticatedNonContractJsonFallsBackWithoutGuessing()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    PolarisResponse nonContract = fixture("capabilities-current.json");
    nonContract = replaceJson(nonContract,
        {{QStringLiteral("server"), QStringLiteral("sunshine")},
         {QStringLiteral("features"), QJsonObject{
             {QStringLiteral("named_commands_v1"), true}}}});
    queue(state, QString::fromLatin1(CapabilitiesRoute), nonContract);

    adapter->pumpCompletions(8);

    QVERIFY(adapter->discoverySnapshot().standardHost);
    QCOMPARE(state->cancellations.size(), 2);
    QCOMPARE(state->requests.size(), 3);
}

void PolarisAdapterTest::completionOrderPublishesOneCoherentGeneration()
{
    QFETCH(QStringList, order);
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();

    for (const QString& endpoint : order) {
        if (endpoint == QString::fromLatin1(CapabilitiesRoute)) {
            queue(state, endpoint, fixture("capabilities-current.json"));
        }
        else if (endpoint == QString::fromLatin1(StatusRoute)) {
            queue(state, endpoint, fixture("status-owner.json"));
        }
        else {
            queue(state, endpoint, fixture("client-settings-current.json"));
        }
        QCOMPARE(adapter->pumpCompletions(1), 1);
        if (endpoint == QString::fromLatin1(CapabilitiesRoute)) {
            QCOMPARE(state->requests.size(), 4);
            QCOMPARE(state->requests.at(3).endpoint,
                     QString::fromLatin1(CommandsRoute));
        }
    }

    QVERIFY(!adapter->discoverySnapshot().complete);
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    QCOMPARE(adapter->pumpCompletions(1), 1);
    const PolarisDiscoverySnapshot snapshot = adapter->discoverySnapshot();
    QVERIFY(snapshot.complete);
    QCOMPARE(snapshot.generation, quint64(1));
    QVERIFY(snapshot.capabilities.valid);
    QCOMPARE(snapshot.capabilities.commands.size(), 2);
    QCOMPARE(snapshot.capabilities.commands.at(0).identifier,
             QStringLiteral("restart-shell"));
    QCOMPARE(snapshot.capabilities.commands.at(1).identifier,
             QStringLiteral("2"));
    QVERIFY(!snapshot.capabilities.commands.at(0).retainsRawCommand);
    QStringList publishedCommandStrings;
    for (const NamedCommandMetadata& command :
         snapshot.capabilities.commands) {
        publishedCommandStrings << command.identifier << command.displayName
                                << command.risk << command.endpoint.advertised
                                << command.endpoint.url.toString(
                                       QUrl::FullyEncoded)
                                << command.endpoint.errorCode;
        QVERIFY(!command.retainsRawCommand);
    }
    QVERIFY(!publishedCommandStrings.join(QLatin1Char('|')).contains(
        QStringLiteral("DO_NOT_RETAIN_RAW_COMMAND_SECRET")));
    QVERIFY(snapshot.session.ownsSession);
    QCOMPARE(snapshot.settings.targets.size(), 6);
}

void PolarisAdapterTest::authenticatedFallbackCancelsSiblingWorkAndKeepsLocalActions()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();

    PolarisResponse notFound;
    notFound.httpStatus = 404;
    notFound.authenticated = true;
    queue(state, QString::fromLatin1(CapabilitiesRoute), notFound);
    QCOMPARE(adapter->pumpCompletions(4), 1);

    const PolarisDiscoverySnapshot snapshot = adapter->discoverySnapshot();
    QVERIFY(snapshot.complete);
    QVERIFY(snapshot.standardHost);
    QCOMPARE(state->cancellations.size(), 2);
    QVERIFY(adapter->snapshot().actionStates.value(
        QStringLiteral("stats.overlay")).enabled);
}

void PolarisAdapterTest::failuresRemainDistinctAndLaterRefreshRecovers_data()
{
    QTest::addColumn<QString>("transportError");
    QTest::addColumn<QString>("expectedCode");
    QTest::newRow("unreachable") << QStringLiteral("network_error")
                                  << QStringLiteral("polaris_unreachable");
    QTest::newRow("timeout") << QStringLiteral("timeout")
                              << QStringLiteral("polaris_unreachable");
    QTest::newRow("tls") << QStringLiteral("tls_identity_mismatch")
                          << QStringLiteral("tls_identity_mismatch");
    QTest::newRow("authentication") << QStringLiteral("authentication_failed")
                                     << QStringLiteral("authentication_failed");
}

void PolarisAdapterTest::failuresRemainDistinctAndLaterRefreshRecovers()
{
    QFETCH(QString, transportError);
    QFETCH(QString, expectedCode);
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    queueCompleteGeneration(state, failure(transportError));
    adapter->pumpCompletions(8);
    QCOMPARE(adapter->discoverySnapshot().errorCode, expectedCode);
    const PolarisAvailability unavailable = adapter->availability(
        PolarisOperation::NamedCommand);
    if (expectedCode == QStringLiteral("polaris_unreachable")) {
        QCOMPARE(unavailable.reason, QStringLiteral("Polaris is unreachable"));
    }
    else {
        QVERIFY(unavailable.reason != QStringLiteral("Polaris is unreachable"));
    }
    QVERIFY(adapter->snapshot().actionStates.value(
        QStringLiteral("stats.overlay")).enabled);

    QVERIFY(adapter->refresh());
    completeGeneration(*adapter, state);
    QVERIFY(adapter->discoverySnapshot().capabilities.valid);
    QCOMPARE(adapter->discoverySnapshot().generation, quint64(2));
}

void PolarisAdapterTest::duplicateCompletionForCurrentGenerationIsIgnored()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    queue(state, QString::fromLatin1(CapabilitiesRoute),
          fixture("capabilities-current.json"));
    queue(state, QString::fromLatin1(CapabilitiesRoute),
          fixture("capabilities-old.json"));
    queue(state, QString::fromLatin1(StatusRoute), fixture("status-owner.json"));
    queue(state, QString::fromLatin1(SettingsRoute),
          fixture("client-settings-current.json"));

    QCOMPARE(adapter->pumpCompletions(8), 4);
    QVERIFY(!adapter->discoverySnapshot().complete);
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    QCOMPARE(adapter->pumpCompletions(8), 2);

    const PolarisDiscoverySnapshot snapshot = adapter->discoverySnapshot();
    QVERIFY(snapshot.complete);
    QVERIFY(snapshot.capabilities.features.contains(
        QStringLiteral("named_commands_v1")));
}

void PolarisAdapterTest::staleAndDuplicateCompletionsCannotReplaceNewerGeneration()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    QVERIFY(adapter->refresh());
    QCOMPARE(state->cancellations.size(), 3);

    queueCompleteGeneration(state, fixture("capabilities-old.json"),
                            fixture("status-viewer.json"),
                            fixture("client-settings-current.json"), 1);
    queueCompleteGeneration(state);
    queue(state, QString::fromLatin1(CapabilitiesRoute),
          fixture("capabilities-old.json"), 1);
    adapter->pumpCompletions(32);
    QVERIFY(!adapter->discoverySnapshot().complete);
    QCOMPARE(state->maximumObservedDrain,
             PolarisAdapter::CompletionPumpLimit);
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    adapter->pumpCompletions(8);

    const PolarisDiscoverySnapshot snapshot = adapter->discoverySnapshot();
    QCOMPARE(snapshot.generation, quint64(2));
    QVERIFY(snapshot.capabilities.features.contains(
        QStringLiteral("named_commands_v1")));
    QVERIFY(snapshot.session.ownsSession);
}

void PolarisAdapterTest::teardownCancelsOutstandingAndLateCallbacksAreHarmless()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    const auto savedCompletion = state->requests.at(0).completion;
    const auto savedId = state->requests.at(0).id;

    adapter.reset();
    QCOMPARE(state->cancellations.size(), 3);
    QCOMPARE(state->destroyed, 1);
    savedCompletion(savedId, fixture("capabilities-current.json"));
    QCOMPARE(state->destroyed, 1);
}

void PolarisAdapterTest::noCommandFollowupForFallbackOldOrRejectedEndpoint_data()
{
    QTest::addColumn<QString>("shape");

    QTest::newRow("old") << QStringLiteral("old");
    QTest::newRow("no-feature") << QStringLiteral("no-feature");
    QTest::newRow("rejected-endpoint") << QStringLiteral("rejected-endpoint");
}

void PolarisAdapterTest::noCommandFollowupForFallbackOldOrRejectedEndpoint()
{
    QFETCH(QString, shape);
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();

    PolarisResponse capabilities = shape == QStringLiteral("old")
        ? fixture("capabilities-old.json")
        : fixture("capabilities-current.json");
    if (shape != QStringLiteral("old")) {
        QJsonObject root = capabilities.json.object();
        if (shape == QStringLiteral("no-feature")) {
            QJsonObject features = root.value(
                QStringLiteral("features")).toObject();
            features.remove(QStringLiteral("named_commands_v1"));
            root.insert(QStringLiteral("features"), features);
        }
        else {
            QJsonObject commands = root.value(
                QStringLiteral("named_commands")).toObject();
            commands.insert(QStringLiteral("endpoint"),
                QStringLiteral("https://DO_NOT_RETAIN.invalid/polaris/v1/commands"));
            root.insert(QStringLiteral("named_commands"), commands);
        }
        capabilities = replaceJson(std::move(capabilities), root);
    }

    queueCompleteGeneration(state, std::move(capabilities));
    adapter->pumpCompletions(8);

    QVERIFY(adapter->discoverySnapshot().complete);
    QCOMPARE(state->requests.size(), 3);
    QCOMPARE(requestId(state, QString::fromLatin1(CommandsRoute)), quint64(0));
}

void PolarisAdapterTest::commandDependencyFailuresDisableOnlyNamedCommands_data()
{
    QTest::addColumn<QString>("transportError");
    QTest::addColumn<int>("httpStatus");
    QTest::addColumn<bool>("authenticated");
    QTest::addColumn<bool>("malformed");
    QTest::addColumn<QString>("expectedCode");
    QTest::addColumn<QString>("expectedReason");

    QTest::newRow("network") << QStringLiteral("network_error") << 0 << false
        << false << QStringLiteral("polaris_unreachable")
        << QStringLiteral("Polaris is unreachable");
    QTest::newRow("timeout") << QStringLiteral("timeout") << 0 << false
        << false << QStringLiteral("polaris_unreachable")
        << QStringLiteral("Polaris is unreachable");
    QTest::newRow("tls") << QStringLiteral("tls_identity_mismatch") << 0 << false
        << false << QStringLiteral("tls_identity_mismatch")
        << QStringLiteral("Polaris identity verification failed");
    QTest::newRow("authentication") << QStringLiteral("http_error") << 401 << true
        << false << QStringLiteral("authentication_failed")
        << QStringLiteral("Polaris authentication failed");
    QTest::newRow("unauthenticated-spoof") << QString() << 200 << false
        << false << QStringLiteral("authentication_failed")
        << QStringLiteral("Polaris authentication failed");
    QTest::newRow("permission") << QStringLiteral("http_error") << 403 << true
        << false << QStringLiteral("permission_denied")
        << QStringLiteral("This paired client lacks permission");
    QTest::newRow("not-found") << QStringLiteral("http_error") << 404 << true
        << false << QStringLiteral("http_error")
        << QStringLiteral("Polaris returned invalid discovery data");
    QTest::newRow("malformed") << QString() << 200 << true << true
        << QStringLiteral("malformed_response")
        << QStringLiteral("Polaris returned invalid discovery data");
}

void PolarisAdapterTest::commandDependencyFailuresDisableOnlyNamedCommands()
{
    QFETCH(QString, transportError);
    QFETCH(int, httpStatus);
    QFETCH(bool, authenticated);
    QFETCH(bool, malformed);
    QFETCH(QString, expectedCode);
    QFETCH(QString, expectedReason);
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    queueCompleteGeneration(state);
    adapter->pumpCompletions(8);

    QCOMPARE(state->requests.size(), 4);
    QVERIFY(!adapter->discoverySnapshot().complete);
    PolarisResponse failed = failure(transportError);
    failed.httpStatus = httpStatus;
    failed.authenticated = authenticated;
    if (malformed) {
        failed.json = QJsonDocument(QJsonArray{1, 2});
    }
    queue(state, QString::fromLatin1(CommandsRoute), std::move(failed));
    adapter->pumpCompletions(8);

    QVERIFY(adapter->discoverySnapshot().complete);
    const PolarisAvailability named = adapter->availability(
        PolarisOperation::NamedCommand);
    QCOMPARE(named.errorCode, expectedCode);
    QCOMPARE(named.reason, expectedReason);
    QVERIFY(adapter->availability(PolarisOperation::ClipboardRead).enabled);
    QVERIFY(adapter->availability(PolarisOperation::StopSession).enabled);
    QVERIFY(adapter->availability(PolarisOperation::DisplaySwitch).enabled);
    QVERIFY(adapter->snapshot().actionStates.value(
        QStringLiteral("stats.overlay")).enabled);
}

void PolarisAdapterTest::refreshAndTeardownOwnOutstandingCommandFollowup()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    queueCompleteGeneration(state);
    adapter->pumpCompletions(8);
    QCOMPARE(state->requests.size(), 4);
    const auto staleCommand = state->requests.at(3).completion;
    const auto staleId = state->requests.at(3).id;

    QVERIFY(adapter->refresh());
    QVERIFY(state->cancellations.contains(staleId));
    staleCommand(staleId, fixture("commands-current.json"));
    staleCommand(staleId, fixture("commands-current.json"));
    queueCompleteGeneration(state);
    adapter->pumpCompletions(8);
    QCOMPARE(state->requests.size(), 8);
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    adapter->pumpCompletions(8);
    QCOMPARE(adapter->discoverySnapshot().generation, quint64(2));
    QCOMPARE(adapter->discoverySnapshot().capabilities.commands.size(), 2);

    auto teardownState = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> teardownGameStream;
    auto teardownAdapter = makeAdapter(
        session, teardownState, &teardownGameStream);
    teardownAdapter->startDiscovery();
    queueCompleteGeneration(teardownState);
    teardownAdapter->pumpCompletions(8);
    QCOMPARE(teardownState->requests.size(), 4);
    const auto lateCommand = teardownState->requests.at(3).completion;
    const auto lateId = teardownState->requests.at(3).id;
    teardownAdapter.reset();
    QVERIFY(teardownState->cancellations.contains(lateId));
    lateCommand(lateId, fixture("commands-current.json"));
    QCOMPARE(teardownState->destroyed, 1);
}

void PolarisAdapterTest::localGameStreamExecutionIsDelegatedUnchanged()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    ActionRegistry registry(PolarisAdapter::descriptors(), *adapter);
    ActionResult result;

    registry.execute(QStringLiteral("stats.overlay"),
                     {{QStringLiteral("enabled"), true}},
                     [&](const ActionResult& completed) { result = completed; });

    QVERIFY(result.ok);
    QVERIFY(session.stats);
    QCOMPARE(result.evidence, QStringLiteral("Statistics overlay: on"));
}

void PolarisAdapterTest::exactDisabledStatesCoverPermissionOwnershipTransitionAndDisplays()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    completeGeneration(*adapter, state, fixture("capabilities-current.json"),
                       fixture("status-viewer.json"),
                       fixture("client-settings-current.json"));

    QCOMPARE(adapter->availability(PolarisOperation::NamedCommand).reason,
             QStringLiteral("This paired client lacks permission"));
    QCOMPARE(adapter->availability(PolarisOperation::StopSession).reason,
             QStringLiteral("This paired client lacks permission"));
    QCOMPARE(adapter->availability(PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("This paired client lacks permission"));

    QJsonObject ownerWithoutOwnership = fixture("status-owner.json").json.object();
    ownerWithoutOwnership.insert(QStringLiteral("owned_by_client"), false);
    adapter->refresh();
    completeGeneration(
        *adapter, state, fixture("capabilities-current.json"),
        replaceJson(fixture("status-owner.json"), ownerWithoutOwnership),
        fixture("client-settings-current.json"));
    QCOMPARE(adapter->availability(PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("Only the controlling client can do this"));

    QJsonObject transitioning = fixture("status-owner.json").json.object();
    transitioning.insert(QStringLiteral("state"), QStringLiteral("stopping"));
    adapter->refresh();
    completeGeneration(
        *adapter, state, fixture("capabilities-current.json"),
        replaceJson(fixture("status-owner.json"), transitioning),
        fixture("client-settings-current.json"));
    QCOMPARE(adapter->availability(PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("The session is transitioning"));

    QJsonObject noAlternate = fixture("client-settings-current.json").json.object();
    QJsonObject settingsCaps = noAlternate.value(QStringLiteral("capabilities")).toObject();
    settingsCaps.insert(QStringLiteral("modes"), QJsonArray{
        QJsonObject{{QStringLiteral("value"), QStringLiteral("desktop_display")},
                    {QStringLiteral("label"), QStringLiteral("Mirror Desktop")},
                    {QStringLiteral("available"), true}}});
    settingsCaps.insert(QStringLiteral("outputs"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("DP-1")},
                    {QStringLiteral("label"), QStringLiteral("Primary")},
                    {QStringLiteral("connected"), true},
                    {QStringLiteral("active"), true}}});
    noAlternate.insert(QStringLiteral("capabilities"), settingsCaps);
    adapter->refresh();
    completeGeneration(
        *adapter, state, fixture("capabilities-current.json"),
        fixture("status-owner.json"),
        replaceJson(fixture("client-settings-current.json"), noAlternate));
    QCOMPARE(adapter->availability(PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("No alternate display is available"));

    adapter->refresh();
    completeGeneration(*adapter, state, fixture("capabilities-old.json"),
                       fixture("status-owner.json"),
                       fixture("client-settings-current.json"));
    QCOMPARE(adapter->availability(PolarisOperation::DisplaySwitch).reason,
             QStringLiteral("This Polaris version does not advertise this feature"));
}

void PolarisAdapterTest::partialMalformedDataDisablesOnlyItsDependentFeature()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();

    QJsonObject capabilities = fixture("capabilities-current.json").json.object();
    QJsonObject commands = capabilities.value(QStringLiteral("named_commands")).toObject();
    commands.insert(QStringLiteral("endpoint"),
                    QStringLiteral("https://evil.invalid/polaris/v1/commands"));
    capabilities.insert(QStringLiteral("named_commands"), commands);
    PolarisResponse malformedSettings;
    malformedSettings.httpStatus = 200;
    malformedSettings.authenticated = true;
    malformedSettings.json = QJsonDocument(QJsonArray{1, 2});
    completeGeneration(*adapter, state,
        replaceJson(fixture("capabilities-current.json"), capabilities),
        fixture("status-owner.json"), malformedSettings);

    QVERIFY(adapter->availability(PolarisOperation::ClipboardRead).enabled);
    QCOMPARE(adapter->availability(PolarisOperation::NamedCommand).reason,
             QStringLiteral("This Polaris version does not advertise this feature"));
    QCOMPARE(adapter->availability(PolarisOperation::DisplaySwitch).errorCode,
             QStringLiteral("malformed_response"));
    QVERIFY(adapter->snapshot().actionStates.value(
        QStringLiteral("stats.overlay")).enabled);
}

void PolarisAdapterTest::deniedCapabilitiesPrecedeValidDocumentFeatureAbsence()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();

    PolarisResponse denied = failure(QStringLiteral("http_error"));
    denied.httpStatus = 403;
    denied.authenticated = true;
    queueCompleteGeneration(state, denied, fixture("status-owner.json"),
                            fixture("client-settings-current.json"));
    adapter->pumpCompletions(8);

    for (const PolarisOperation operation : {
             PolarisOperation::ClipboardRead,
             PolarisOperation::ClipboardWrite,
             PolarisOperation::NamedCommand,
             PolarisOperation::StopSession,
             PolarisOperation::DisplaySwitch}) {
        const PolarisAvailability result = adapter->availability(operation);
        QCOMPARE(result.errorCode, QStringLiteral("permission_denied"));
        QCOMPARE(result.reason,
                 QStringLiteral("This paired client lacks permission"));
    }
}

void PolarisAdapterTest::dependencyFailuresRemainExactAndCompositional_data()
{
    QTest::addColumn<bool>("sessionFailure");
    QTest::addColumn<QString>("transportError");
    QTest::addColumn<int>("httpStatus");
    QTest::addColumn<QString>("expectedCode");
    QTest::addColumn<QString>("expectedReason");

    QTest::newRow("session-network") << true << QStringLiteral("network_error")
        << 0 << QStringLiteral("polaris_unreachable")
        << QStringLiteral("Polaris is unreachable");
    QTest::newRow("session-timeout") << true << QStringLiteral("timeout")
        << 0 << QStringLiteral("polaris_unreachable")
        << QStringLiteral("Polaris is unreachable");
    QTest::newRow("session-tls") << true << QStringLiteral("tls_identity_mismatch")
        << 0 << QStringLiteral("tls_identity_mismatch")
        << QStringLiteral("Polaris identity verification failed");
    QTest::newRow("session-auth") << true << QStringLiteral("http_error")
        << 401 << QStringLiteral("authentication_failed")
        << QStringLiteral("Polaris authentication failed");
    QTest::newRow("session-permission") << true << QStringLiteral("http_error")
        << 403 << QStringLiteral("permission_denied")
        << QStringLiteral("This paired client lacks permission");
    QTest::newRow("settings-network") << false << QStringLiteral("network_error")
        << 0 << QStringLiteral("polaris_unreachable")
        << QStringLiteral("Polaris is unreachable");
    QTest::newRow("settings-tls") << false << QStringLiteral("tls_identity_mismatch")
        << 0 << QStringLiteral("tls_identity_mismatch")
        << QStringLiteral("Polaris identity verification failed");
}

void PolarisAdapterTest::dependencyFailuresRemainExactAndCompositional()
{
    QFETCH(bool, sessionFailure);
    QFETCH(QString, transportError);
    QFETCH(int, httpStatus);
    QFETCH(QString, expectedCode);
    QFETCH(QString, expectedReason);
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    PolarisResponse failed = failure(transportError);
    failed.httpStatus = httpStatus;
    failed.authenticated = httpStatus != 0;
    completeGeneration(
        *adapter, state,
        fixture("capabilities-current.json"),
        sessionFailure ? failed : fixture("status-owner.json"),
        sessionFailure ? fixture("client-settings-current.json") : failed);

    const PolarisAvailability display = adapter->availability(
        PolarisOperation::DisplaySwitch);
    QCOMPARE(display.errorCode, expectedCode);
    QCOMPARE(display.reason, expectedReason);
    if (!sessionFailure) {
        QVERIFY(adapter->availability(PolarisOperation::ClipboardRead).enabled);
        QVERIFY(adapter->availability(PolarisOperation::NamedCommand).enabled);
        QVERIFY(adapter->availability(PolarisOperation::StopSession).enabled);
    }
    else {
        QCOMPARE(adapter->availability(PolarisOperation::ClipboardRead).errorCode,
                 expectedCode);
        QCOMPARE(adapter->availability(PolarisOperation::NamedCommand).errorCode,
                 expectedCode);
        QCOMPARE(adapter->availability(PolarisOperation::StopSession).errorCode,
                 expectedCode);
    }
}

void PolarisAdapterTest::refreshKeepsPreviousSnapshotUntilNewGenerationIsComplete()
{
    FakeSession session;
    auto state = std::make_shared<FakeTransportState>();
    std::unique_ptr<GameStreamAdapter> gameStream;
    auto adapter = makeAdapter(session, state, &gameStream);
    adapter->startDiscovery();
    completeGeneration(*adapter, state);
    const PolarisDiscoverySnapshot first = adapter->discoverySnapshot();
    QCOMPARE(first.generation, quint64(1));

    adapter->refresh();
    queueCompleteGeneration(state, fixture("capabilities-current.json"),
                            fixture("status-viewer.json"),
                            fixture("client-settings-current.json"));
    adapter->pumpCompletions(8);
    QCOMPARE(adapter->discoverySnapshot().generation, quint64(1));
    QVERIFY(adapter->discoverySnapshot().capabilities.features.contains(
        QStringLiteral("named_commands_v1")));

    QCOMPARE(state->requests.last().endpoint,
             QString::fromLatin1(CommandsRoute));
    queue(state, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    adapter->pumpCompletions(8);
    QCOMPARE(adapter->discoverySnapshot().generation, quint64(2));
    QVERIFY(!adapter->discoverySnapshot().session.ownsSession);
    QCOMPARE(adapter->discoverySnapshot().capabilities.commands.size(), 2);
}

REGISTER_PERIGEE_TEST(PolarisAdapterTest);

#include "test_polarisadapter.moc"
