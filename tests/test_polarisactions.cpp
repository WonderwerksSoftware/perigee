#include "perigee/actions/actionregistry.h"
#include "perigee/actions/gamestreamadapter.h"
#include "perigee/actions/sessionfacade.h"
#include "perigee/polaris/polarisadapter.h"
#include "test_registry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

constexpr auto CapabilitiesRoute = "/polaris/v1/capabilities";
constexpr auto StatusRoute = "/polaris/v1/session/status";
constexpr auto SettingsRoute = "/polaris/v1/client-settings";
constexpr auto CommandsRoute = "/polaris/v1/commands";
constexpr auto BitrateRoute = "/polaris/v1/session/bitrate";
constexpr auto AdaptiveRoute = "/polaris/v1/session/adaptive-bitrate";

PolarisResponse fixture(const char* name, int status = 200)
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
    response.authenticated = true;
    return response;
}

PolarisResponse jsonResponse(const QJsonObject& object, int status = 200)
{
    PolarisResponse response;
    response.httpStatus = status;
    response.json = QJsonDocument(object);
    response.body = response.json.toJson(QJsonDocument::Compact);
    response.authenticated = true;
    return response;
}

PolarisResponse capabilitiesWithClipboardLimit(qint64 limit)
{
    QJsonObject root = fixture("capabilities-current.json").json.object();
    QJsonObject clipboard = root.value(QStringLiteral("clipboard")).toObject();
    clipboard.insert(QStringLiteral("max_text_bytes"), limit);
    root.insert(QStringLiteral("clipboard"), clipboard);
    return jsonResponse(root);
}

PolarisResponse statusWithClipboardPermissions(bool readAllowed,
                                                bool writeAllowed)
{
    QJsonObject root = fixture("status-owner.json").json.object();
    QJsonObject controls = root.value(QStringLiteral("controls")).toObject();
    controls.insert(QStringLiteral("clipboard_read_allowed"), readAllowed);
    controls.insert(QStringLiteral("clipboard_write_allowed"), writeAllowed);
    root.insert(QStringLiteral("controls"), controls);
    return jsonResponse(root);
}

PolarisResponse statusWithoutSessionToken()
{
    QJsonObject root = fixture("status-owner.json").json.object();
    root.remove(QStringLiteral("session_token"));
    return jsonResponse(root);
}

class FakeSession final : public SessionFacade
{
public:
    bool statsOverlayEnabled() const override { return false; }
    bool mouseCaptureEnabled() const override { return false; }
    bool keyboardCaptureEnabled() const override { return false; }
    bool fullscreenEnabled() const override { return false; }
    int configuredBitrateKbps() const override { return 35000; }
    bool setStatsOverlayEnabled(bool) override { return true; }
    bool setMouseCaptureEnabled(bool) override { return true; }
    bool setKeyboardCaptureEnabled(bool) override { return true; }
    bool setFullscreenEnabled(bool) override { return true; }
    int physicalDisplayCount() const override { return 3; }
    int lastRequestedPhysicalDisplay() const override { return 0; }
    bool physicalDisplaySwitchActive() const override { return false; }
    bool requestPhysicalDisplay(int, PhysicalDisplayCompletion) override
    {
        return false;
    }
    bool requestClientDisconnect() override { ++disconnects; return true; }
    bool requestPerigeeQuit() override { ++quits; return true; }

    int disconnects = 0;
    int quits = 0;
};

struct FakeTransportState
{
    struct Request {
        PolarisTransport::RequestId id = 0;
        QString endpoint;
        QByteArray method;
        QByteArray body;
        bool expectJson = false;
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
    std::optional<PolarisResponse> synchronousClipboardFetch;
    int activeTransportCalls = 0;
    bool destroyedDuringTransportCall = false;
};

struct FakeClipboardState
{
    QByteArray localText;
    QByteArray writtenText;
    int readCalls = 0;
    int writeCalls = 0;
    int wipeCalls = 0;
    bool readSucceeds = true;
    bool writeSucceeds = true;
    bool allObservedBytesWereZero = true;
    QVector<Qt::HANDLE> callThreads;
    std::function<void()> onWipe;
};

class FakeClipboard final : public PolarisClipboard
{
public:
    explicit FakeClipboard(std::shared_ptr<FakeClipboardState> state)
        : m_State(std::move(state)) {}

    bool readText(QByteArray* text) override
    {
        ++m_State->readCalls;
        m_State->callThreads.push_back(QThread::currentThreadId());
        if (m_State->readSucceeds && text != nullptr) {
            *text = m_State->localText;
        }
        return m_State->readSucceeds;
    }

    bool writeText(const QByteArray& text) override
    {
        ++m_State->writeCalls;
        m_State->callThreads.push_back(QThread::currentThreadId());
        if (m_State->writeSucceeds) {
            m_State->writtenText = text;
        }
        return m_State->writeSucceeds;
    }

    void sensitiveBufferWiped(const QByteArray& bytes) override
    {
        ++m_State->wipeCalls;
        for (const char byte : bytes) {
            m_State->allObservedBytesWereZero &= byte == '\0';
        }
        std::function<void()> onWipe = std::move(m_State->onWipe);
        m_State->onWipe = {};
        if (onWipe) {
            onWipe();
        }
    }

private:
    std::shared_ptr<FakeClipboardState> m_State;
};

std::unique_ptr<PolarisClipboard> makeClipboard(
    const std::shared_ptr<FakeClipboardState>& state)
{
    return state == nullptr
        ? std::unique_ptr<PolarisClipboard>()
        : std::unique_ptr<PolarisClipboard>(new FakeClipboard(state));
}

class FakeTransport final : public PolarisTransport
{
public:
    explicit FakeTransport(std::shared_ptr<FakeTransportState> state)
        : m_State(std::move(state)) {}

    ~FakeTransport() override
    {
        if (m_State->activeTransportCalls > 0) {
            m_State->destroyedDuringTransportCall = true;
        }
    }

    RequestId get(const QString& endpoint, bool expectJson, Completion completion,
                  PolarisRequestOptions) override
    {
        const RequestId id = m_State->nextId++;
        m_State->requests.push_back({id, endpoint, QByteArrayLiteral("GET"),
                                    {}, expectJson, std::move(completion)});
        return id;
    }

    RequestId post(const QString& endpoint, const QByteArray& body,
                   bool expectJson, Completion completion,
                   PolarisRequestOptions) override
    {
        const RequestId id = m_State->nextId++;
        m_State->requests.push_back({id, endpoint, QByteArrayLiteral("POST"),
                                    body, expectJson, std::move(completion)});
        return id;
    }

    RequestId fetchClipboard(std::optional<qint64>, Completion completion,
                             PolarisRequestOptions) override
    {
        const std::shared_ptr<FakeTransportState> state = m_State;
        ++state->activeTransportCalls;
        const RequestId id = state->nextId++;
        state->requests.push_back(
            {id, QStringLiteral("/actions/clipboard?type=text"),
             QByteArrayLiteral("GET"), {}, false, completion});
        if (state->synchronousClipboardFetch.has_value()) {
            completion(id, *state->synchronousClipboardFetch);
        }
        --state->activeTransportCalls;
        return id;
    }

    RequestId sendClipboard(const QByteArray& body, Completion completion,
                            PolarisRequestOptions) override
    {
        const RequestId id = m_State->nextId++;
        m_State->requests.push_back(
            {id, QStringLiteral("/actions/clipboard?type=text"),
             QByteArrayLiteral("POST"), body, false,
             std::move(completion)});
        return id;
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
        const std::shared_ptr<FakeTransportState> state = m_State;
        ++state->activeTransportCalls;
        int delivered = 0;
        while (delivered < maximum && !state->deliveries.isEmpty()) {
            const auto delivery = state->deliveries.dequeue();
            const auto request = std::find_if(
                state->requests.cbegin(), state->requests.cend(),
                [&delivery](const auto& candidate) {
                    return candidate.id == delivery.id;
                });
            if (request != state->requests.cend() && request->completion) {
                request->completion(delivery.id, delivery.response);
            }
            ++delivered;
        }
        --state->activeTransportCalls;
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

struct Harness
{
    explicit Harness(
        std::shared_ptr<FakeClipboardState> clipboardState = {})
        : transport(std::make_shared<FakeTransportState>())
        , clipboard(std::move(clipboardState))
        , local(std::make_unique<GameStreamAdapter>(&session))
        , adapter(std::make_unique<PolarisAdapter>(
              *local, std::make_unique<FakeTransport>(transport),
              makeClipboard(clipboard)))
    {
    }

    void complete(
        PolarisResponse commands = fixture("commands-current.json"),
        PolarisResponse capabilities = fixture("capabilities-current.json"),
        PolarisResponse sessionStatus = fixture("status-owner.json"))
    {
        QVERIFY(adapter->startDiscovery() || adapter->refresh());
        queue(transport, QString::fromLatin1(CapabilitiesRoute),
              std::move(capabilities));
        queue(transport, QString::fromLatin1(StatusRoute),
              std::move(sessionStatus));
        queue(transport, QString::fromLatin1(SettingsRoute),
              fixture("client-settings-current.json"));
        QCOMPARE(adapter->pumpCompletions(8), 3);
        queue(transport, QString::fromLatin1(CommandsRoute),
              std::move(commands));
        QCOMPARE(adapter->pumpCompletions(8), 1);
        QVERIFY(adapter->discoverySnapshot().complete);
    }

    FakeSession session;
    std::shared_ptr<FakeTransportState> transport;
    std::shared_ptr<FakeClipboardState> clipboard;
    std::unique_ptr<GameStreamAdapter> local;
    std::unique_ptr<PolarisAdapter> adapter;
};

QStringList ids(const QVector<ActionDescriptor>& descriptors)
{
    QStringList result;
    for (const auto& descriptor : descriptors) {
        result.append(descriptor.id);
    }
    return result;
}

}

class PolarisActionsTest final : public QObject
{
    Q_OBJECT

private slots:
    void registersFixedActionsAndHiddenCommandTemplate();
    void exposesOfficialQualityStateAndMetadata();
    void sendsBoundedQualityRequestsAndRefreshesReadback();
    void clampsQualityRequestsAndRejectsMismatchedReadback();
    void qualityControlsFailClosedWithoutHostPermission();
    void expandsCommandsInNumericOrderWithoutTemplateRow();
    void refreshRemovesStaleCommandsAndSearchesByDisplayName();
    void commandRiskFailsClosedAndDirectExecutionCannotBypassConfirmation();
    void acceptedCommandAndConfirmedStopCompleteWithoutLocalExit();
    void catalogChangeInvalidatesPendingCommandConfirmation();
    void sendsLocalClipboardThroughExactBoundedRouteAndWipesOwnedBytes();
    void rejectsInvalidAndOversizedLocalClipboardBeforeTransport();
    void fetchesRemoteClipboardWithoutPublishingItsContents();
    void clipboardSendBoundaries_data();
    void clipboardSendBoundaries();
    void clipboardFetchBoundaries_data();
    void clipboardFetchBoundaries();
    void enforcesClipboardPermissionSplit();
    void mapsCommandAndStopTerminalStatuses();
    void rejectsDuplicateCancelsAndTeardownSafely();
    void handlesSynchronousAndWrongThreadClipboardCompletions();
    void survivesAdapterDestructionFromPumpedCompletion();
    void survivesAdapterDestructionFromSynchronousCompletion();
    void permitsReentrantSensitiveObserversDuringCleanup();
    void wipesSynchronousEarlyResponseWhenCompletionThrows();
    void commandAndStopLifetimesAndMissingTokenFailClosed();
};

void PolarisActionsTest::registersFixedActionsAndHiddenCommandTemplate()
{
    const QVector<ActionDescriptor> descriptors = PolarisAdapter::descriptors();
    const QStringList descriptorIds = ids(descriptors);
    QVERIFY(descriptorIds.contains(QStringLiteral("clipboard.send-local")));
    QVERIFY(descriptorIds.contains(QStringLiteral("clipboard.fetch-remote")));
    QVERIFY(descriptorIds.contains(QStringLiteral("session.end-host")));
    QVERIFY(descriptorIds.contains(QStringLiteral("host.command")));
    QVERIFY(descriptorIds.contains(QStringLiteral("quality.mode.manual")));
    QVERIFY(descriptorIds.contains(QStringLiteral("quality.mode.adaptive")));
    QVERIFY(descriptorIds.contains(QStringLiteral("quality.bitrate.decrease")));
    QVERIFY(descriptorIds.contains(QStringLiteral("quality.bitrate.increase")));

    const auto stop = std::find_if(descriptors.cbegin(), descriptors.cend(),
        [](const ActionDescriptor& descriptor) {
            return descriptor.id == QStringLiteral("session.end-host");
        });
    QVERIFY(stop != descriptors.cend());
    QCOMPARE(stop->category, ActionCategory::Session);
    QCOMPARE(stop->resourceKey, QStringLiteral("session.lifecycle"));
    QCOMPARE(stop->confirmation, ConfirmationPolicy::Always);
    QVERIFY(stop->confirmationMessage.contains(
        QStringLiteral("all connected clients"), Qt::CaseInsensitive));
    QVERIFY(stop->confirmationMessage.contains(
        QStringLiteral("different from disconnecting this client"),
        Qt::CaseInsensitive));
}

void PolarisActionsTest::exposesOfficialQualityStateAndMetadata()
{
    Harness harness;
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    const QVector<ActionDescriptor> qualityActions =
        registry.actions(ActionCategory::Quality);
    const QStringList actionIds = ids(qualityActions);
    QVERIFY(actionIds.contains(QStringLiteral("quality.status")));
    QVERIFY(actionIds.contains(QStringLiteral("quality.mode.manual")));
    QVERIFY(actionIds.contains(QStringLiteral("quality.mode.adaptive")));
    QVERIFY(actionIds.contains(QStringLiteral("quality.bitrate.decrease")));
    QVERIFY(actionIds.contains(QStringLiteral("quality.bitrate.increase")));

    const ActionState status = registry.state(QStringLiteral("quality.status"));
    QVERIFY(!status.enabled);
    QCOMPARE(status.value.toString(), QStringLiteral("Adaptive · 28 Mbps"));
    QCOMPARE(registry.state(QStringLiteral("quality.mode.manual")).value.toString(),
             QString());
    QCOMPARE(registry.state(QStringLiteral("quality.mode.adaptive")).value.toString(),
             QStringLiteral("Selected"));
    QVERIFY(registry.state(QStringLiteral("quality.bitrate.decrease")).enabled);
    QVERIFY(registry.state(QStringLiteral("quality.bitrate.increase")).enabled);
}

void PolarisActionsTest::sendsBoundedQualityRequestsAndRefreshesReadback()
{
    Harness harness;
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    ActionResult manual;
    registry.execute(QStringLiteral("quality.mode.manual"), {},
                     [&](const ActionResult& result) { manual = result; });
    QCOMPARE(harness.transport->requests.last().endpoint,
             QString::fromLatin1(AdaptiveRoute));
    QCOMPARE(harness.transport->requests.last().body,
             QByteArrayLiteral("{\"enabled\":false}"));
    queue(harness.transport, QString::fromLatin1(AdaptiveRoute),
          jsonResponse({{QStringLiteral("status"), true},
                        {QStringLiteral("ai_auto_quality_enabled"), false},
                        {QStringLiteral("adaptive_bitrate_enabled"), false},
                        {QStringLiteral("ai_optimizer_enabled"), false}}));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);
    QVERIFY(manual.ok);
    QCOMPARE(harness.transport->requests.last().endpoint,
             QString::fromLatin1(SettingsRoute));

    queue(harness.transport, QString::fromLatin1(CapabilitiesRoute),
          fixture("capabilities-current.json"));
    queue(harness.transport, QString::fromLatin1(StatusRoute),
          fixture("status-owner.json"));
    queue(harness.transport, QString::fromLatin1(SettingsRoute),
          fixture("client-settings-current.json"));
    QCOMPARE(harness.adapter->pumpCompletions(8), 3);
    queue(harness.transport, QString::fromLatin1(CommandsRoute),
          fixture("commands-current.json"));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);

    ActionResult increase;
    registry.execute(QStringLiteral("quality.bitrate.increase"), {},
                     [&](const ActionResult& result) { increase = result; });
    QCOMPARE(harness.transport->requests.last().endpoint,
             QString::fromLatin1(BitrateRoute));
    QCOMPARE(harness.transport->requests.last().body,
             QByteArrayLiteral("{\"bitrate_kbps\":40000}"));
    queue(harness.transport, QString::fromLatin1(BitrateRoute),
          jsonResponse({{QStringLiteral("status"), true},
                        {QStringLiteral("bitrate_kbps"), 40000}}));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);
    QVERIFY(increase.ok);
}

void PolarisActionsTest::qualityControlsFailClosedWithoutHostPermission()
{
    QJsonObject status = fixture("status-owner.json").json.object();
    QJsonObject controls = status.value(QStringLiteral("controls")).toObject();
    controls.insert(QStringLiteral("host_tuning_allowed"), false);
    status.insert(QStringLiteral("controls"), controls);

    Harness harness;
    harness.complete(fixture("commands-current.json"),
                     fixture("capabilities-current.json"),
                     jsonResponse(status));
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    for (const QString& id : {
             QStringLiteral("quality.mode.manual"),
             QStringLiteral("quality.mode.adaptive"),
             QStringLiteral("quality.bitrate.decrease"),
             QStringLiteral("quality.bitrate.increase")}) {
        const ActionState state = registry.state(id);
        QVERIFY(!state.enabled);
        QCOMPARE(state.disabledCode, QStringLiteral("permission_denied"));
    }
}

void PolarisActionsTest::clampsQualityRequestsAndRejectsMismatchedReadback()
{
    QJsonObject status = fixture("status-owner.json").json.object();
    QJsonObject tuning = status.value(QStringLiteral("tuning")).toObject();
    tuning.insert(QStringLiteral("adaptive_base_bitrate_kbps"), 98000);
    tuning.insert(QStringLiteral("adaptive_max_bitrate_kbps"), 100000);
    status.insert(QStringLiteral("tuning"), tuning);

    Harness harness;
    harness.complete(fixture("commands-current.json"),
                     fixture("capabilities-current.json"),
                     jsonResponse(status));
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    ActionResult result;
    registry.execute(QStringLiteral("quality.bitrate.increase"), {},
                     [&](const ActionResult& completed) { result = completed; });
    QCOMPARE(harness.transport->requests.last().body,
             QByteArrayLiteral("{\"bitrate_kbps\":100000}"));
    queue(harness.transport, QString::fromLatin1(BitrateRoute),
          jsonResponse({{QStringLiteral("status"), true},
                        {QStringLiteral("bitrate_kbps"), 99000}}));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("malformed_response"));
    QCOMPARE(harness.transport->requests.last().endpoint,
             QString::fromLatin1(BitrateRoute));
}

void PolarisActionsTest::expandsCommandsInNumericOrderWithoutTemplateRow()
{
    Harness harness;
    harness.complete(jsonResponse({
        {QStringLiteral("version"), 1},
        {QStringLiteral("commands"), QJsonArray{
             QJsonObject{{QStringLiteral("index"), 10},
                         {QStringLiteral("name"), QStringLiteral("Ten")},
                         {QStringLiteral("risk"), QStringLiteral("safe")}},
             QJsonObject{{QStringLiteral("index"), 0},
                         {QStringLiteral("name"), QStringLiteral("Restart shell")},
                         {QStringLiteral("risk"), QStringLiteral("disruptive")}},
             QJsonObject{{QStringLiteral("index"), 2},
                         {QStringLiteral("name"), QStringLiteral("Clear cache")},
                         {QStringLiteral("risk"), QStringLiteral("safe")}}
         }}
    }));
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    const QVector<ActionDescriptor> sessionActions =
        registry.actions(ActionCategory::Session);
    const QStringList actionIds = ids(sessionActions);
    QCOMPARE(actionIds.count(QStringLiteral("host.command")), 0);
    const int zero = actionIds.indexOf(QStringLiteral("host.command.0"));
    const int two = actionIds.indexOf(QStringLiteral("host.command.2"));
    const int ten = actionIds.indexOf(QStringLiteral("host.command.10"));
    QVERIFY(zero >= 0);
    QVERIFY(two > zero);
    QVERIFY(ten > two);
    QCOMPARE(registry.state(QStringLiteral("host.command.0"))
                 .value.toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Restart shell"));
    QCOMPARE(registry.state(QStringLiteral("host.command.2"))
                 .value.toMap().value(QStringLiteral("index")).toInt(), 2);
}

void PolarisActionsTest::refreshRemovesStaleCommandsAndSearchesByDisplayName()
{
    Harness harness;
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    QCOMPARE(ids(registry.search(QStringLiteral("clear cache"))),
             QStringList({QStringLiteral("host.command.2")}));

    harness.complete(jsonResponse({
        {QStringLiteral("version"), 1},
        {QStringLiteral("commands"), QJsonArray{
             QJsonObject{{QStringLiteral("index"), 7},
                         {QStringLiteral("id"), QStringLiteral("new")},
                         {QStringLiteral("name"), QStringLiteral("New command")},
                         {QStringLiteral("risk"), QStringLiteral("safe")}}
         }}
    }));

    const QStringList refreshed = ids(registry.actions(ActionCategory::Session));
    QVERIFY(!refreshed.contains(QStringLiteral("host.command.0")));
    QVERIFY(!refreshed.contains(QStringLiteral("host.command.2")));
    QVERIFY(refreshed.contains(QStringLiteral("host.command.7")));
}

void PolarisActionsTest::commandRiskFailsClosedAndDirectExecutionCannotBypassConfirmation()
{
    Harness harness;
    harness.complete(jsonResponse({
        {QStringLiteral("version"), 1},
        {QStringLiteral("commands"), QJsonArray{
             QJsonObject{{QStringLiteral("index"), 1},
                         {QStringLiteral("id"), QStringLiteral("known-safe")},
                         {QStringLiteral("name"), QStringLiteral("Known safe")},
                         {QStringLiteral("risk"), QStringLiteral("safe")}},
             QJsonObject{{QStringLiteral("index"), 2},
                         {QStringLiteral("id"), QStringLiteral("danger")},
                         {QStringLiteral("name"), QStringLiteral("Danger")},
                         {QStringLiteral("risk"), QStringLiteral("disruptive")}},
             QJsonObject{{QStringLiteral("index"), 3},
                         {QStringLiteral("id"), QStringLiteral("future")},
                         {QStringLiteral("name"), QStringLiteral("Future")},
                         {QStringLiteral("risk"), QStringLiteral("future-risk")}}
         }}
    }));
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    QVERIFY(!registry.requiresConfirmation(QStringLiteral("host.command.1")));
    QVERIFY(registry.requiresConfirmation(QStringLiteral("host.command.2")));
    QVERIFY(registry.requiresConfirmation(QStringLiteral("host.command.3")));

    ActionResult result;
    registry.execute(QStringLiteral("host.command.2"), {},
                     [&](const ActionResult& value) { result = value; });
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("confirmation_required"));
    QCOMPARE(harness.transport->requests.size(), 4);
}

void PolarisActionsTest::acceptedCommandAndConfirmedStopCompleteWithoutLocalExit()
{
    Harness harness;
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    ActionResult command;
    registry.execute(QStringLiteral("host.command.2"), {},
                     [&](const ActionResult& value) { command = value; });
    QCOMPARE(harness.transport->requests.last().endpoint,
             QString::fromLatin1(CommandsRoute));
    QCOMPARE(harness.transport->requests.last().method,
             QByteArrayLiteral("POST"));
    QCOMPARE(harness.transport->requests.last().body,
             QByteArrayLiteral("{\"index\":2,\"session_token\":\"fixture-owner-token\"}"));
    queue(harness.transport, QString::fromLatin1(CommandsRoute),
          jsonResponse({{QStringLiteral("accepted"), true},
                        {QStringLiteral("state"), QStringLiteral("accepted")}},
                       202));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);
    QVERIFY(command.ok);
    QCOMPARE(command.evidence, QStringLiteral("Accepted by Polaris"));
    const ActionState publishedCommand =
        registry.state(QStringLiteral("host.command.2"));
    QCOMPARE(publishedCommand.phase, ActionPhase::Succeeded);
    QCOMPARE(publishedCommand.message, QStringLiteral("Accepted by Polaris"));
    QVERIFY(!publishedCommand.value.toMap().contains(
        QStringLiteral("clipboard")));
    QCOMPARE(harness.transport->requests.size(), 8);

    ActionResult directStop;
    registry.execute(QStringLiteral("session.end-host"), {},
                     [&](const ActionResult& value) { directStop = value; });
    QVERIFY(!directStop.ok);
    QCOMPARE(directStop.errorCode, QStringLiteral("confirmation_required"));
    QVERIFY(registry.beginConfirmation(QStringLiteral("session.end-host")));
    ActionResult stop;
    registry.acceptConfirmation(
        QStringLiteral("session.end-host"), {},
        [&](const ActionResult& value) { stop = value; });
    QCOMPARE(harness.transport->requests.last().endpoint,
             QStringLiteral("/polaris/v1/session/stop"));
    QCOMPARE(harness.transport->requests.last().body,
             QByteArrayLiteral("{\"session_token\":\"fixture-owner-token\"}"));
    queue(harness.transport, QStringLiteral("/polaris/v1/session/stop"),
          jsonResponse({{QStringLiteral("status"), true}}));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);
    QVERIFY(stop.ok);
    QCOMPARE(registry.state(QStringLiteral("session.end-host")).phase,
             ActionPhase::Succeeded);
    QCOMPARE(harness.session.disconnects, 0);
    QCOMPARE(harness.session.quits, 0);
}

void PolarisActionsTest::catalogChangeInvalidatesPendingCommandConfirmation()
{
    Harness harness;
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    QVERIFY(registry.beginConfirmation(QStringLiteral("host.command.0")));

    harness.complete(jsonResponse({
        {QStringLiteral("version"), 1},
        {QStringLiteral("commands"), QJsonArray{
             QJsonObject{{QStringLiteral("index"), 0},
                         {QStringLiteral("id"), QStringLiteral("different")},
                         {QStringLiteral("name"), QStringLiteral("Different command")},
                         {QStringLiteral("risk"), QStringLiteral("disruptive")}}
         }}
    }));
    ActionResult result;
    registry.acceptConfirmation(
        QStringLiteral("host.command.0"), {},
        [&](const ActionResult& value) { result = value; });

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("state_changed"));
    QCOMPARE(harness.transport->requests.size(), 8);
}

void PolarisActionsTest::sendsLocalClipboardThroughExactBoundedRouteAndWipesOwnedBytes()
{
    auto clipboard = std::make_shared<FakeClipboardState>();
    clipboard->localText = QByteArray::fromHex("e99baae29883");
    const QByteArray canary = clipboard->localText;
    Harness harness(clipboard);
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);

    ActionResult result;
    registry.execute(QStringLiteral("clipboard.send-local"), {},
                     [&](const ActionResult& value) { result = value; });
    QCOMPARE(harness.transport->requests.last().endpoint,
             QStringLiteral("/actions/clipboard?type=text"));
    QCOMPARE(harness.transport->requests.last().method,
             QByteArrayLiteral("POST"));
    QCOMPARE(harness.transport->requests.last().body, canary);
    queue(harness.transport, QStringLiteral("/actions/clipboard?type=text"),
          PolarisResponse{204, {}, {}, {}, {}, true});
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);

    QVERIFY(result.ok);
    QCOMPARE(result.evidence, QStringLiteral("Clipboard sent to host"));
    QVERIFY(!result.evidence.contains(QString::fromUtf8(canary)));
    QVERIFY(result.observedState.has_value());
    QVERIFY(!result.observedState->value.isValid());
    QCOMPARE(clipboard->readCalls, 1);
    QVERIFY(clipboard->wipeCalls >= 1);
    QVERIFY(clipboard->allObservedBytesWereZero);
}

void PolarisActionsTest::rejectsInvalidAndOversizedLocalClipboardBeforeTransport()
{
    auto clipboard = std::make_shared<FakeClipboardState>();
    Harness harness(clipboard);
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    const int discoveryRequests = harness.transport->requests.size();

    clipboard->localText = QByteArray::fromHex("c328");
    ActionResult invalid;
    registry.execute(QStringLiteral("clipboard.send-local"), {},
                     [&](const ActionResult& value) { invalid = value; });
    QVERIFY(!invalid.ok);
    QCOMPARE(invalid.errorCode, QStringLiteral("invalid_utf8"));
    QCOMPARE(harness.transport->requests.size(), discoveryRequests);

    clipboard->localText = QByteArray(262145, 'x');
    ActionResult oversized;
    registry.execute(QStringLiteral("clipboard.send-local"), {},
                     [&](const ActionResult& value) { oversized = value; });
    QVERIFY(!oversized.ok);
    QCOMPARE(oversized.errorCode, QStringLiteral("clipboard_too_large"));
    QCOMPARE(harness.transport->requests.size(), discoveryRequests);
    QVERIFY(clipboard->wipeCalls >= 2);
    QVERIFY(clipboard->allObservedBytesWereZero);
}

void PolarisActionsTest::fetchesRemoteClipboardWithoutPublishingItsContents()
{
    auto clipboard = std::make_shared<FakeClipboardState>();
    Harness harness(clipboard);
    harness.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    const QByteArray remote = QByteArrayLiteral("REMOTE_CANARY_14");

    ActionResult result;
    registry.execute(QStringLiteral("clipboard.fetch-remote"), {},
                     [&](const ActionResult& value) { result = value; });
    QCOMPARE(harness.transport->requests.last().endpoint,
             QStringLiteral("/actions/clipboard?type=text"));
    QCOMPARE(harness.transport->requests.last().method,
             QByteArrayLiteral("GET"));
    PolarisResponse response;
    response.httpStatus = 200;
    response.body = remote;
    response.authenticated = true;
    queue(harness.transport, QStringLiteral("/actions/clipboard?type=text"),
          response);
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);

    QVERIFY(result.ok);
    QCOMPARE(result.evidence, QStringLiteral("Clipboard copied from host"));
    QVERIFY(!result.evidence.contains(QString::fromUtf8(remote)));
    QCOMPARE(clipboard->writtenText, remote);
    QCOMPARE(clipboard->writeCalls, 1);
    QVERIFY(clipboard->wipeCalls >= 1);
    QVERIFY(clipboard->allObservedBytesWereZero);
}

void PolarisActionsTest::clipboardSendBoundaries_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<qint64>("limit");
    QTest::addColumn<bool>("accepted");
    QTest::addColumn<QString>("errorCode");

    QTest::newRow("empty") << QByteArray() << qint64(32) << true << QString();
    QTest::newRow("ascii") << QByteArrayLiteral("hello") << qint64(32)
                            << true << QString();
    QTest::newRow("utf8") << QByteArray::fromHex("e99baae29883")
                           << qint64(32) << true << QString();
    QTest::newRow("embedded-nul") << QByteArray("a\0b", 3) << qint64(32)
                                   << false << QStringLiteral("invalid_utf8");
    QTest::newRow("invalid-utf8") << QByteArray::fromHex("c328")
                                  << qint64(32) << false
                                  << QStringLiteral("invalid_utf8");
    QTest::newRow("one-below") << QByteArray(31, 'a') << qint64(32)
                                << true << QString();
    QTest::newRow("exact") << QByteArray(32, 'a') << qint64(32)
                            << true << QString();
    QTest::newRow("one-over") << QByteArray(33, 'a') << qint64(32)
                               << false << QStringLiteral("clipboard_too_large");
    QTest::newRow("lower-exact") << QByteArray(4, 'a') << qint64(4)
                                  << true << QString();
    QTest::newRow("lower-over") << QByteArray(5, 'a') << qint64(4)
                                 << false << QStringLiteral("clipboard_too_large");
    QTest::newRow("absolute-exact") << QByteArray(1024 * 1024, 'a')
                                     << qint64(1024 * 1024)
                                     << true << QString();
    QTest::newRow("absolute-over") << QByteArray(1024 * 1024 + 1, 'a')
                                    << qint64(1024 * 1024 + 100)
                                    << false
                                    << QStringLiteral("clipboard_too_large");
}

void PolarisActionsTest::clipboardSendBoundaries()
{
    QFETCH(QByteArray, text);
    QFETCH(qint64, limit);
    QFETCH(bool, accepted);
    QFETCH(QString, errorCode);
    auto clipboard = std::make_shared<FakeClipboardState>();
    clipboard->localText = text;
    Harness harness(clipboard);
    harness.complete(fixture("commands-current.json"),
                     capabilitiesWithClipboardLimit(limit));
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    const int before = harness.transport->requests.size();
    ActionResult result;

    registry.execute(QStringLiteral("clipboard.send-local"), {},
                     [&](const ActionResult& value) { result = value; });
    if (accepted) {
        QCOMPARE(harness.transport->requests.size(), before + 1);
        queue(harness.transport, QStringLiteral("/actions/clipboard?type=text"),
              PolarisResponse{204, {}, {}, {}, {}, true});
        QCOMPARE(harness.adapter->pumpCompletions(8), 1);
        QVERIFY(result.ok);
        QCOMPARE(result.evidence, QStringLiteral("Clipboard sent to host"));
    }
    else {
        QCOMPARE(harness.transport->requests.size(), before);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, errorCode);
    }
    QVERIFY(clipboard->allObservedBytesWereZero);
    QVERIFY(clipboard->wipeCalls >= 1);
}

void PolarisActionsTest::clipboardFetchBoundaries_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<qint64>("limit");
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("transportError");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("empty") << QByteArray() << qint64(8) << 200
                            << QString() << QString();
    QTest::newRow("utf8") << QByteArray::fromHex("e99baae29883")
                           << qint64(8) << 200 << QString() << QString();
    QTest::newRow("invalid") << QByteArray::fromHex("c328") << qint64(8)
                              << 200 << QString()
                              << QStringLiteral("invalid_utf8");
    QTest::newRow("nul") << QByteArray("a\0b", 3) << qint64(8) << 200
                          << QString() << QStringLiteral("invalid_utf8");
    QTest::newRow("exact") << QByteArray(8, 'a') << qint64(8) << 200
                            << QString() << QString();
    QTest::newRow("lower-exact") << QByteArray(3, 'a') << qint64(3) << 200
                                  << QString() << QString();
    QTest::newRow("over-fake-transport") << QByteArray(9, 'a') << qint64(8)
                                          << 200 << QString()
                                          << QStringLiteral("clipboard_too_large");
    QTest::newRow("http-413") << QByteArray() << qint64(8) << 413
                               << QString()
                               << QStringLiteral("clipboard_too_large");
    QTest::newRow("transport-cap") << QByteArray() << qint64(8) << 200
                                    << QStringLiteral("response_too_large")
                                    << QStringLiteral("clipboard_too_large");
}

void PolarisActionsTest::clipboardFetchBoundaries()
{
    QFETCH(QByteArray, text);
    QFETCH(qint64, limit);
    QFETCH(int, status);
    QFETCH(QString, transportError);
    QFETCH(QString, expectedError);
    auto clipboard = std::make_shared<FakeClipboardState>();
    Harness harness(clipboard);
    harness.complete(fixture("commands-current.json"),
                     capabilitiesWithClipboardLimit(limit));
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    ActionResult result;
    registry.execute(QStringLiteral("clipboard.fetch-remote"), {},
                     [&](const ActionResult& value) { result = value; });
    PolarisResponse response;
    response.httpStatus = status;
    response.body = text;
    response.errorCode = transportError;
    response.authenticated = true;
    queue(harness.transport, QStringLiteral("/actions/clipboard?type=text"),
          std::move(response));
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);

    if (expectedError.isEmpty()) {
        QVERIFY(result.ok);
        QCOMPARE(clipboard->writtenText, text);
        QCOMPARE(clipboard->writeCalls, 1);
    }
    else {
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, expectedError);
        QCOMPARE(clipboard->writeCalls, 0);
    }
    QVERIFY(clipboard->allObservedBytesWereZero);
    QVERIFY(clipboard->wipeCalls >= 1);
}

void PolarisActionsTest::enforcesClipboardPermissionSplit()
{
    auto firstClipboard = std::make_shared<FakeClipboardState>();
    Harness first(firstClipboard);
    first.complete(fixture("commands-current.json"),
                   fixture("capabilities-current.json"),
                   statusWithClipboardPermissions(true, false));
    ActionRegistry firstRegistry(PolarisAdapter::descriptors(), *first.adapter);
    ActionResult deniedSend;
    firstRegistry.execute(QStringLiteral("clipboard.send-local"), {},
        [&](const ActionResult& value) { deniedSend = value; });
    QVERIFY(!deniedSend.ok);
    QCOMPARE(firstClipboard->readCalls, 0);
    firstRegistry.execute(QStringLiteral("clipboard.fetch-remote"), {}, {});
    QCOMPARE(first.transport->requests.last().method, QByteArrayLiteral("GET"));

    auto secondClipboard = std::make_shared<FakeClipboardState>();
    secondClipboard->localText = QByteArrayLiteral("allowed");
    Harness second(secondClipboard);
    second.complete(fixture("commands-current.json"),
                    fixture("capabilities-current.json"),
                    statusWithClipboardPermissions(false, true));
    ActionRegistry secondRegistry(PolarisAdapter::descriptors(), *second.adapter);
    secondRegistry.execute(QStringLiteral("clipboard.send-local"), {}, {});
    QCOMPARE(secondClipboard->readCalls, 1);
    ActionResult deniedFetch;
    secondRegistry.execute(QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult& value) { deniedFetch = value; });
    QVERIFY(!deniedFetch.ok);
    QCOMPARE(secondClipboard->writeCalls, 0);
}

void PolarisActionsTest::mapsCommandAndStopTerminalStatuses()
{
    const QVector<QPair<PolarisResponse, QString>> commandCases = {
        {PolarisResponse{400, {}, {}, {}, {}, true}, QStringLiteral("invalid_request")},
        {PolarisResponse{401, {}, {}, {}, {}, true}, QStringLiteral("authentication_failed")},
        {PolarisResponse{403, {}, {}, {}, {}, true}, QStringLiteral("permission_denied")},
        {PolarisResponse{409, {}, {}, {}, {}, true}, QStringLiteral("stale_session")},
        {PolarisResponse{500, {}, {}, {}, {}, true}, QStringLiteral("host_operation_failed")},
        {PolarisResponse{0, {}, {}, QStringLiteral("timeout"), {}, false}, QStringLiteral("timeout")},
        {PolarisResponse{403, {}, {}, QStringLiteral("tls_identity_mismatch"),
                         QStringLiteral("Pinned identity mismatch"), false},
         QStringLiteral("tls_identity_mismatch")},
        {jsonResponse({{QStringLiteral("accepted"), true},
                       {QStringLiteral("state"), QStringLiteral("accepted")}}, 200),
         QStringLiteral("malformed_response")},
        {jsonResponse({}, 202), QStringLiteral("malformed_response")},
    };
    for (const auto& terminal : commandCases) {
        Harness harness;
        harness.complete();
        ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
        ActionResult result;
        registry.execute(QStringLiteral("host.command.2"), {},
            [&](const ActionResult& value) { result = value; });
        queue(harness.transport, QString::fromLatin1(CommandsRoute), terminal.first);
        QCOMPARE(harness.adapter->pumpCompletions(8), 1);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, terminal.second);
    }

    const QVector<QPair<PolarisResponse, QString>> stopCases = {
        {PolarisResponse{403, {}, {}, {}, {}, true}, QStringLiteral("permission_denied")},
        {PolarisResponse{409, {}, {}, {}, {}, true}, QStringLiteral("stale_session")},
        {PolarisResponse{470, {}, {}, {}, {}, true}, QStringLiteral("session_not_owned")},
        {PolarisResponse{500, {}, {}, {}, {}, true}, QStringLiteral("host_operation_failed")},
        {PolarisResponse{0, {}, {}, QStringLiteral("network_error"), {}, false}, QStringLiteral("network_error")},
        {jsonResponse({}, 200), QStringLiteral("malformed_response")},
    };
    for (const auto& terminal : stopCases) {
        Harness harness;
        harness.complete();
        ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
        QVERIFY(registry.beginConfirmation(QStringLiteral("session.end-host")));
        ActionResult result;
        registry.acceptConfirmation(QStringLiteral("session.end-host"), {},
            [&](const ActionResult& value) { result = value; });
        queue(harness.transport, QStringLiteral("/polaris/v1/session/stop"),
              terminal.first);
        QCOMPARE(harness.adapter->pumpCompletions(8), 1);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, terminal.second);
        QCOMPARE(harness.session.disconnects, 0);
        QCOMPARE(harness.session.quits, 0);
    }
}

void PolarisActionsTest::rejectsDuplicateCancelsAndTeardownSafely()
{
    Harness duplicate;
    duplicate.complete();
    ActionRegistry registry(PolarisAdapter::descriptors(), *duplicate.adapter);
    int completions = 0;
    registry.execute(QStringLiteral("host.command.2"), {},
        [&](const ActionResult&) { ++completions; });
    const PolarisResponse accepted = jsonResponse({
        {QStringLiteral("accepted"), true},
        {QStringLiteral("state"), QStringLiteral("accepted")}}, 202);
    queue(duplicate.transport, QString::fromLatin1(CommandsRoute), accepted);
    queue(duplicate.transport, QString::fromLatin1(CommandsRoute), accepted);
    QCOMPARE(duplicate.adapter->pumpCompletions(8), 2);
    QCOMPARE(completions, 1);

    auto clipboard = std::make_shared<FakeClipboardState>();
    clipboard->localText = QByteArrayLiteral("CANCEL_TEARDOWN_CANARY");
    Harness cancelled(clipboard);
    cancelled.complete();
    ActionRegistry cancelledRegistry(PolarisAdapter::descriptors(),
                                     *cancelled.adapter);
    cancelledRegistry.execute(QStringLiteral("clipboard.send-local"), {}, {});
    const auto actionId = cancelled.transport->requests.last().id;
    cancelled.adapter->cancel(QStringLiteral("clipboard"));
    QVERIFY(cancelled.transport->cancellations.contains(actionId));
    PolarisResponse cancelledResponse;
    cancelledResponse.errorCode = QStringLiteral("cancelled");
    queue(cancelled.transport, QStringLiteral("/actions/clipboard?type=text"),
          cancelledResponse);
    QCOMPARE(cancelled.adapter->pumpCompletions(8), 1);
    QVERIFY(clipboard->allObservedBytesWereZero);

    auto teardownClipboard = std::make_shared<FakeClipboardState>();
    teardownClipboard->localText = QByteArrayLiteral("TEARDOWN_CANARY");
    Harness teardown(teardownClipboard);
    teardown.complete();
    ActionRegistry teardownRegistry(PolarisAdapter::descriptors(),
                                    *teardown.adapter);
    teardownRegistry.execute(QStringLiteral("clipboard.send-local"), {}, {});
    const auto teardownId = teardown.transport->requests.last().id;
    teardown.adapter.reset();
    QVERIFY(teardown.transport->cancellations.contains(teardownId));
    QVERIFY(teardownClipboard->allObservedBytesWereZero);
}

void PolarisActionsTest::handlesSynchronousAndWrongThreadClipboardCompletions()
{
    auto synchronousClipboard = std::make_shared<FakeClipboardState>();
    Harness synchronous(synchronousClipboard);
    synchronous.complete();
    PolarisResponse synchronousResponse;
    synchronousResponse.httpStatus = 200;
    synchronousResponse.body = QByteArrayLiteral("SYNC_FETCH_CANARY");
    synchronousResponse.authenticated = true;
    synchronous.transport->synchronousClipboardFetch = synchronousResponse;
    ActionRegistry synchronousRegistry(PolarisAdapter::descriptors(),
                                       *synchronous.adapter);
    ActionResult synchronousResult;
    synchronousRegistry.execute(QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult& value) { synchronousResult = value; });
    QVERIFY(synchronousResult.ok);
    QCOMPARE(synchronousClipboard->writtenText,
             QByteArrayLiteral("SYNC_FETCH_CANARY"));
    QVERIFY(synchronousClipboard->wipeCalls >= 2);
    QVERIFY(synchronousClipboard->allObservedBytesWereZero);

    auto wrongThreadClipboard = std::make_shared<FakeClipboardState>();
    Harness wrongThread(wrongThreadClipboard);
    wrongThread.complete();
    ActionRegistry wrongThreadRegistry(PolarisAdapter::descriptors(),
                                       *wrongThread.adapter);
    ActionResult wrongThreadResult;
    wrongThreadRegistry.execute(QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult& value) { wrongThreadResult = value; });
    const auto pending = wrongThread.transport->requests.last();
    PolarisResponse response;
    response.httpStatus = 200;
    response.body = QByteArrayLiteral("WRONG_THREAD_CANARY");
    response.authenticated = true;
    std::thread completionThread([pending, response] {
        pending.completion(pending.id, response);
    });
    completionThread.join();
    QVERIFY(!wrongThreadResult.ok);
    QCOMPARE(wrongThreadResult.errorCode, QStringLiteral("wrong_thread"));
    QCOMPARE(wrongThreadClipboard->writeCalls, 0);
    QVERIFY(wrongThreadClipboard->allObservedBytesWereZero);

    auto wrongSendClipboard = std::make_shared<FakeClipboardState>();
    wrongSendClipboard->localText = QByteArrayLiteral("MUST_NOT_BE_READ");
    Harness wrongSend(wrongSendClipboard);
    wrongSend.complete();
    ActionRegistry wrongSendRegistry(PolarisAdapter::descriptors(),
                                     *wrongSend.adapter);
    ActionResult wrongSendResult;
    std::thread sendThread([&] {
        wrongSendRegistry.execute(QStringLiteral("clipboard.send-local"), {},
            [&](const ActionResult& value) { wrongSendResult = value; });
    });
    sendThread.join();
    QVERIFY(!wrongSendResult.ok);
    QCOMPARE(wrongSendResult.errorCode, QStringLiteral("wrong_thread"));
    QCOMPARE(wrongSendClipboard->readCalls, 0);
}

void PolarisActionsTest::survivesAdapterDestructionFromPumpedCompletion()
{
    Harness pumped;
    pumped.complete();
    ActionRegistry pumpedRegistry(PolarisAdapter::descriptors(),
                                  *pumped.adapter);
    int pumpedCompletions = 0;
    pumpedRegistry.execute(QStringLiteral("host.command.2"), {},
        [&](const ActionResult&) {
            ++pumpedCompletions;
            pumped.adapter.reset();
        });
    queue(pumped.transport, QString::fromLatin1(CommandsRoute),
          jsonResponse({{QStringLiteral("accepted"), true},
                        {QStringLiteral("state"), QStringLiteral("accepted")}},
                       202));
    PolarisAdapter* pumpedRaw = pumped.adapter.get();
    QCOMPARE(pumpedRaw->pumpCompletions(8), 1);
    QVERIFY(pumped.adapter == nullptr);
    QCOMPARE(pumpedCompletions, 1);
    QVERIFY(!pumped.transport->destroyedDuringTransportCall);
}

void PolarisActionsTest::survivesAdapterDestructionFromSynchronousCompletion()
{
    auto clipboard = std::make_shared<FakeClipboardState>();
    Harness synchronous(clipboard);
    synchronous.complete();
    PolarisResponse response;
    response.httpStatus = 200;
    response.body = QByteArrayLiteral("SYNC_DESTROY_CANARY");
    response.authenticated = true;
    synchronous.transport->synchronousClipboardFetch = response;
    ActionRegistry synchronousRegistry(PolarisAdapter::descriptors(),
                                       *synchronous.adapter);
    int synchronousCompletions = 0;
    synchronousRegistry.execute(QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult&) {
            ++synchronousCompletions;
            synchronous.adapter.reset();
        });
    QVERIFY(synchronous.adapter == nullptr);
    QCOMPARE(synchronousCompletions, 1);
    QVERIFY(!synchronous.transport->destroyedDuringTransportCall);
    QVERIFY(clipboard->allObservedBytesWereZero);
}

void PolarisActionsTest::permitsReentrantSensitiveObserversDuringCleanup()
{
    auto cancelClipboard = std::make_shared<FakeClipboardState>();
    cancelClipboard->localText = QByteArrayLiteral("REENTRANT_CANCEL_CANARY");
    Harness cancelled(cancelClipboard);
    cancelled.complete();
    ActionRegistry cancelledRegistry(PolarisAdapter::descriptors(),
                                     *cancelled.adapter);
    cancelledRegistry.execute(QStringLiteral("clipboard.send-local"), {}, {});
    PolarisAdapter* cancelRaw = cancelled.adapter.get();
    int cancelReentries = 0;
    cancelClipboard->onWipe = [cancelRaw, &cancelReentries] {
        cancelRaw->discoverySnapshot();
        ++cancelReentries;
    };
    cancelled.adapter->cancel(QStringLiteral("clipboard"));
    QCOMPARE(cancelReentries, 1);
    QVERIFY(cancelClipboard->allObservedBytesWereZero);

    auto teardownClipboard = std::make_shared<FakeClipboardState>();
    teardownClipboard->localText = QByteArrayLiteral("REENTRANT_TEARDOWN_CANARY");
    Harness teardown(teardownClipboard);
    teardown.complete();
    ActionRegistry teardownRegistry(PolarisAdapter::descriptors(),
                                    *teardown.adapter);
    teardownRegistry.execute(QStringLiteral("clipboard.send-local"), {}, {});
    PolarisAdapter* teardownRaw = teardown.adapter.get();
    int teardownReentries = 0;
    teardownClipboard->onWipe = [teardownRaw, &teardownReentries] {
        teardownRaw->discoverySnapshot();
        ++teardownReentries;
    };
    teardown.adapter.reset();
    QCOMPARE(teardownReentries, 1);
    QVERIFY(teardownClipboard->allObservedBytesWereZero);

    auto earlyClipboard = std::make_shared<FakeClipboardState>();
    Harness early(earlyClipboard);
    early.complete();
    PolarisResponse earlyResponse;
    earlyResponse.httpStatus = 200;
    earlyResponse.body = QByteArrayLiteral("REENTRANT_EARLY_CANARY");
    earlyResponse.authenticated = true;
    early.transport->synchronousClipboardFetch = earlyResponse;
    ActionRegistry earlyRegistry(PolarisAdapter::descriptors(), *early.adapter);
    PolarisAdapter* earlyRaw = early.adapter.get();
    int earlyReentries = 0;
    earlyRegistry.execute(QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult&) {
            earlyClipboard->onWipe = [earlyRaw, &earlyReentries] {
                earlyRaw->discoverySnapshot();
                ++earlyReentries;
            };
        });
    QCOMPARE(earlyReentries, 1);
    QVERIFY(earlyClipboard->allObservedBytesWereZero);
}

void PolarisActionsTest::wipesSynchronousEarlyResponseWhenCompletionThrows()
{
    auto clipboard = std::make_shared<FakeClipboardState>();
    Harness harness(clipboard);
    harness.complete();
    PolarisResponse response;
    response.httpStatus = 200;
    response.body = QByteArrayLiteral("THROWING_EARLY_CANARY");
    response.authenticated = true;
    harness.transport->synchronousClipboardFetch = response;
    ActionRegistry registry(PolarisAdapter::descriptors(), *harness.adapter);
    int completionCalls = 0;
    bool threw = false;
    try {
        registry.execute(QStringLiteral("clipboard.fetch-remote"), {},
            [&](const ActionResult&) {
                ++completionCalls;
                throw std::runtime_error("expected completion failure");
            });
    }
    catch (const std::runtime_error&) {
        threw = true;
    }
    QVERIFY(threw);
    QCOMPARE(completionCalls, 1);
    QVERIFY(clipboard->wipeCalls >= 3);
    QVERIFY(clipboard->allObservedBytesWereZero);

    queue(harness.transport, QStringLiteral("/actions/clipboard?type=text"),
          response);
    QCOMPARE(harness.adapter->pumpCompletions(8), 1);
    QCOMPARE(completionCalls, 1);
}

void PolarisActionsTest::commandAndStopLifetimesAndMissingTokenFailClosed()
{
    Harness missingToken;
    missingToken.complete(fixture("commands-current.json"),
                          fixture("capabilities-current.json"),
                          statusWithoutSessionToken());
    ActionRegistry missingRegistry(PolarisAdapter::descriptors(),
                                   *missingToken.adapter);
    const int discoveryCount = missingToken.transport->requests.size();
    ActionResult missingResult;
    missingRegistry.execute(QStringLiteral("host.command.2"), {},
        [&](const ActionResult& value) { missingResult = value; });
    QVERIFY(!missingResult.ok);
    QCOMPARE(missingToken.transport->requests.size(), discoveryCount);

    Harness commandCancel;
    commandCancel.complete();
    ActionRegistry commandCancelRegistry(PolarisAdapter::descriptors(),
                                         *commandCancel.adapter);
    commandCancelRegistry.execute(QStringLiteral("host.command.2"), {}, {});
    const auto commandCancelId = commandCancel.transport->requests.last().id;
    commandCancel.adapter->cancel(QStringLiteral("host.command"));
    QVERIFY(commandCancel.transport->cancellations.contains(commandCancelId));

    Harness commandTeardown;
    commandTeardown.complete();
    ActionRegistry commandTeardownRegistry(PolarisAdapter::descriptors(),
                                           *commandTeardown.adapter);
    commandTeardownRegistry.execute(QStringLiteral("host.command.2"), {}, {});
    const auto commandTeardownId = commandTeardown.transport->requests.last().id;
    commandTeardown.adapter.reset();
    QVERIFY(commandTeardown.transport->cancellations.contains(commandTeardownId));

    Harness stopDuplicate;
    stopDuplicate.complete();
    ActionRegistry stopDuplicateRegistry(PolarisAdapter::descriptors(),
                                         *stopDuplicate.adapter);
    QVERIFY(stopDuplicateRegistry.beginConfirmation(
        QStringLiteral("session.end-host")));
    int stopCompletions = 0;
    stopDuplicateRegistry.acceptConfirmation(
        QStringLiteral("session.end-host"), {},
        [&](const ActionResult&) { ++stopCompletions; });
    const PolarisResponse stopped = jsonResponse({{QStringLiteral("status"), true}});
    queue(stopDuplicate.transport, QStringLiteral("/polaris/v1/session/stop"),
          stopped);
    queue(stopDuplicate.transport, QStringLiteral("/polaris/v1/session/stop"),
          stopped);
    QCOMPARE(stopDuplicate.adapter->pumpCompletions(8), 2);
    QCOMPARE(stopCompletions, 1);

    Harness stopCancel;
    stopCancel.complete();
    ActionRegistry stopCancelRegistry(PolarisAdapter::descriptors(),
                                      *stopCancel.adapter);
    QVERIFY(stopCancelRegistry.beginConfirmation(QStringLiteral("session.end-host")));
    stopCancelRegistry.acceptConfirmation(QStringLiteral("session.end-host"), {}, {});
    const auto stopCancelId = stopCancel.transport->requests.last().id;
    stopCancel.adapter->cancel(QStringLiteral("session.lifecycle"));
    QVERIFY(stopCancel.transport->cancellations.contains(stopCancelId));

    Harness stopTeardown;
    stopTeardown.complete();
    ActionRegistry stopTeardownRegistry(PolarisAdapter::descriptors(),
                                        *stopTeardown.adapter);
    QVERIFY(stopTeardownRegistry.beginConfirmation(
        QStringLiteral("session.end-host")));
    stopTeardownRegistry.acceptConfirmation(
        QStringLiteral("session.end-host"), {}, {});
    const auto stopTeardownId = stopTeardown.transport->requests.last().id;
    stopTeardown.adapter.reset();
    QVERIFY(stopTeardown.transport->cancellations.contains(stopTeardownId));
}

REGISTER_PERIGEE_TEST(PolarisActionsTest);

#include "test_polarisactions.moc"
