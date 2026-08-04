#include "support/fakepolarisserver.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"
#include "perigee/actions/actionregistry.h"
#include "perigee/actions/gamestreamadapter.h"
#include "perigee/actions/sessionfacade.h"
#include "perigee/display/sessiontransitioncoordinator.h"
#include "perigee/polaris/polarisadapter.h"
#include "perigee/polaris/polarisapiclient.h"
#include "test_registry.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

class PolarisIntegrationTest final : public QObject
{
    Q_OBJECT

private slots:
    void harnessListensOnlyOnEphemeralIpv4Loopback();
    void realQtBackendUsesMutualTlsAndExactPinnedLeaf();
    void teardownWipesDelayedResponseAndCompletesRequestOnce();
    void authenticatedRawParserRejectsMalformedRequests_data();
    void authenticatedRawParserRejectsMalformedRequests();
    void currentDiscoveryPublishesPolarisActionsAndDynamicTargets();
    void oldMalformedAndNonPolarisDiscoveryFailClosed();
    void wrongServerLeafAndClientIdentityFailClosed();
    void authenticatedHttpTimeoutDropAndPolicyRemainDistinct();
    void partialUnknownDiscoveryAndClipboardPermissionsStayIndependent();
    void publicActionsMapAuthenticatedFailuresExactly();
    void namedCommandsUseMetadataConfirmationAndAuthenticatedAcks();
    void inPlaceDisplaySwitchRequiresFreshReadbackAndFrame();
    void displayDisagreementRollsBackExactlyOnce();
    void reconnectDisplaySwitchSucceedsWithoutStoppingHost();
    void reconnectVerificationDeadlineRollsBackExactlyOnce();
    void reconnectFailureRollsBackAndRollbackFailureExposesRecovery();
    void clipboardLimitsUtf8AcknowledgementAndLogsStaySafe();
};

namespace
{
NvComputer pairedComputer(const FakePolarisServer& server)
{
    NvComputer computer;
    computer.activeAddress = NvAddress(
        QHostAddress(QHostAddress::LocalHost), server.port());
    computer.activeHttpsPort = server.port();
    computer.serverCert = server.serverCertificate();
    return computer;
}

QByteArray fixture(const QString& name)
{
    QFile file(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
        QStringLiteral("fixtures/polaris/%1").arg(name)));
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Unable to open integration fixture");
    }
    return file.readAll();
}

QByteArray tlsFixture(const QString& name)
{
    QFile file(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
        QStringLiteral("fixtures/tls/%1").arg(name)));
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Unable to open TLS integration fixture");
    }
    return file.readAll();
}

QByteArray pemPayload(QByteArray pem)
{
    QList<QByteArray> lines = pem.split('\n');
    QByteArray payload;
    for (QByteArray& line : lines) {
        line = line.trimmed();
        if (!line.isEmpty() && !line.startsWith("-----")) {
            payload.append(line);
        }
    }
    return payload;
}

FakePolarisServer::ResponseScript jsonScript(
    const QString& path, const QString& fixtureName, int status = 200)
{
    FakePolarisServer::ResponseScript script;
    script.path = path;
    script.status = status;
    script.headers.insert(QByteArrayLiteral("Content-Type"),
                          QByteArrayLiteral("application/json"));
    script.bodyChunks = {fixture(fixtureName)};
    return script;
}

FakePolarisServer::ResponseScript jsonBodyScript(
    const QString& path, QByteArray body, int status = 200)
{
    FakePolarisServer::ResponseScript script;
    script.path = path;
    script.status = status;
    script.headers.insert(QByteArrayLiteral("Content-Type"),
                          QByteArrayLiteral("application/json"));
    script.bodyChunks = {std::move(body)};
    return script;
}

class RealTlsTransport final : public PolarisTransport
{
public:
    RealTlsTransport(const NvComputer& computer,
                     const QSslConfiguration& identity)
        : m_Client(std::make_unique<PolarisApiClient>(
              computer, PolarisApiClient::TestIdentityTag{}, identity)) {}

    RequestId get(const QString& endpoint, bool expectJson,
                  Completion completion,
                  PolarisRequestOptions options) override
    {
        return m_Client->get(endpoint, expectJson,
                             std::move(completion), options);
    }

    RequestId post(const QString& endpoint, const QByteArray& body,
                   bool expectJson, Completion completion,
                   PolarisRequestOptions options) override
    {
        return m_Client->request(QByteArrayLiteral("POST"), endpoint, body,
                                 expectJson, std::move(completion), options);
    }

    RequestId fetchClipboard(std::optional<qint64> advertisedLimit,
                             Completion completion,
                             PolarisRequestOptions options) override
    {
        return m_Client->fetchClipboard(advertisedLimit,
                                        std::move(completion), options);
    }

    RequestId sendClipboard(const QByteArray& body, Completion completion,
                            PolarisRequestOptions options) override
    {
        return m_Client->sendClipboard(body, std::move(completion), options);
    }

    bool cancel(RequestId requestId) override
    {
        return m_Client->cancel(requestId);
    }

    int drainCompletions(int maximum) override
    {
        return m_Client->drainCompletions(maximum);
    }

    qsizetype pendingCompletionCount() const override
    {
        return m_Client->pendingCompletionCount();
    }

    QUrl pairedOrigin() const override
    {
        return m_Client->origin();
    }

private:
    std::unique_ptr<PolarisApiClient> m_Client;
};

class IntegrationSession final : public SessionFacade
{
public:
    bool statsOverlayEnabled() const override { return stats; }
    bool mouseCaptureEnabled() const override { return mouse; }
    bool keyboardCaptureEnabled() const override { return keyboard; }
    bool fullscreenEnabled() const override { return fullscreen; }
    bool setStatsOverlayEnabled(bool value) override { stats = value; return true; }
    bool setMouseCaptureEnabled(bool value) override { mouse = value; return true; }
    bool setKeyboardCaptureEnabled(bool value) override { keyboard = value; return true; }
    bool setFullscreenEnabled(bool value) override { fullscreen = value; return true; }
    bool requestClientDisconnect() override { return true; }
    bool requestPerigeeQuit() override { return true; }

    bool stats = false;
    bool mouse = false;
    bool keyboard = false;
    bool fullscreen = false;
};

class IntegrationClipboard final : public PolarisClipboard
{
public:
    bool readText(QByteArray* text) override
    {
        if (text == nullptr || !readSucceeds) {
            return false;
        }
        *text = localText;
        return true;
    }

    bool writeText(const QByteArray& text) override
    {
        if (!writeSucceeds) {
            return false;
        }
        remoteText = text;
        return true;
    }

    void sensitiveBufferWiped(const QByteArray& bytes) override
    {
        ++wipeCount;
        for (char byte : bytes) {
            allWipedBytesZero &= byte == '\0';
        }
    }

    QByteArray localText;
    QByteArray remoteText;
    bool readSucceeds = true;
    bool writeSucceeds = true;
    int wipeCount = 0;
    bool allWipedBytesZero = true;
};

struct AdapterHarness
{
    explicit AdapterHarness(
        FakePolarisServer& server,
        SessionTransitionCoordinator* transitionCoordinator = nullptr)
        : local(&session)
        , clipboard(new IntegrationClipboard())
        , clipboardView(clipboard.get())
        , adapter(local,
                  std::make_unique<RealTlsTransport>(
                      pairedComputer(server), server.clientSslConfiguration()),
                  std::move(clipboard), transitionCoordinator)
        , registry(PolarisAdapter::descriptors(), adapter)
    {
    }

    IntegrationSession session;
    GameStreamAdapter local;
    std::unique_ptr<IntegrationClipboard> clipboard;
    IntegrationClipboard* clipboardView;
    PolarisAdapter adapter;
    ActionRegistry registry;
};

class IntegrationDisplayPort final : public DisplayTransitionPort
{
public:
    explicit IntegrationDisplayPort(PolarisAdapter* adapter,
                                    bool refreshAutomatically = true)
        : m_Adapter(adapter)
        , m_RefreshAutomatically(refreshAutomatically)
    {
    }

    void postTarget(const DisplayTarget& target, const QString& sessionToken,
                    quint64 transactionEpoch,
                    PostCompletion completion) override
    {
        if (m_Adapter != nullptr) {
            m_Adapter->postDisplayTarget(
                target, sessionToken, transactionEpoch,
                std::move(completion));
        }
    }

    void postAuthorizedTarget(
        const DisplayTarget& target, const QString& sessionToken,
        quint64 transactionEpoch,
        const PolarisDiscoverySnapshot& authorizationSnapshot,
        PostCompletion completion) override
    {
        if (m_Adapter != nullptr) {
            m_Adapter->postDisplayTarget(
                target, sessionToken, transactionEpoch,
                authorizationSnapshot, std::move(completion));
        }
    }

    bool requestLocalDisconnect(quint64 transactionEpoch) override
    {
        disconnectEpochs.push_back(transactionEpoch);
        return disconnectSucceeds;
    }

    void armFirstFrameEvidence(quint64 evidenceEpoch) override
    {
        evidenceEpochs.push_back(evidenceEpoch);
    }

    void refreshReadback(quint64 transactionEpoch) override
    {
        refreshEpochs.push_back(transactionEpoch);
        if (m_Adapter != nullptr && m_RefreshAutomatically) {
            m_Adapter->refresh();
        }
    }

    QVector<quint64> disconnectEpochs;
    QVector<quint64> evidenceEpochs;
    QVector<quint64> refreshEpochs;
    bool disconnectSucceeds = true;

private:
    PolarisAdapter* m_Adapter = nullptr;
    bool m_RefreshAutomatically = true;
};

void enqueueCurrentDiscovery(FakePolarisServer& server)
{
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/capabilities"),
                              QStringLiteral("capabilities-current.json")));
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                              QStringLiteral("status-owner.json")));
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/client-settings"),
                              QStringLiteral("client-settings-current.json")));
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/commands"),
                              QStringLiteral("commands-current.json")));
}

QByteArray displaySettings(const QString& currentOutput,
                           bool requiresReconnect)
{
    QJsonObject root = QJsonDocument::fromJson(
        fixture(QStringLiteral("client-settings-current.json"))).object();
    QJsonObject effective = root.value(QStringLiteral("effective")).toObject();
    effective.insert(QStringLiteral("output_name"), currentOutput);
    root.insert(QStringLiteral("effective"), effective);
    QJsonObject desired = root.value(QStringLiteral("desired")).toObject();
    desired.insert(QStringLiteral("output_name"), currentOutput);
    root.insert(QStringLiteral("desired"), desired);
    QJsonObject fields = root.value(QStringLiteral("fields")).toObject();
    QJsonObject outputField = fields.value(
        QStringLiteral("output_name")).toObject();
    outputField.insert(QStringLiteral("desired"), currentOutput);
    outputField.insert(QStringLiteral("effective"), currentOutput);
    outputField.insert(QStringLiteral("requires_reconnect"),
                       requiresReconnect);
    fields.insert(QStringLiteral("output_name"), outputField);
    root.insert(QStringLiteral("fields"), fields);
    QJsonObject capabilities = root.value(
        QStringLiteral("capabilities")).toObject();
    QJsonArray outputs = capabilities.value(
        QStringLiteral("outputs")).toArray();
    for (qsizetype index = 0; index < outputs.size(); ++index) {
        QJsonObject output = outputs.at(index).toObject();
        const QString id = output.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            output.insert(QStringLiteral("active"), id == currentOutput);
            output.insert(QStringLiteral("requires_reconnect"),
                          requiresReconnect);
            outputs.replace(index, output);
        }
    }
    capabilities.insert(QStringLiteral("outputs"), outputs);
    root.insert(QStringLiteral("capabilities"), capabilities);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void enqueueDiscoveryWithSettings(FakePolarisServer& server,
                                  QByteArray settings)
{
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/capabilities"),
                              QStringLiteral("capabilities-current.json")));
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                              QStringLiteral("status-owner.json")));
    server.enqueue(jsonBodyScript(QStringLiteral("/polaris/v1/client-settings"),
                                  std::move(settings)));
    server.enqueue(jsonScript(QStringLiteral("/polaris/v1/commands"),
                              QStringLiteral("commands-current.json")));
}

FakePolarisServer::ResponseScript displayPost(
    const QString& output, bool* exactBody, int status = 204)
{
    FakePolarisServer::ResponseScript script;
    script.method = QByteArrayLiteral("POST");
    script.path = QStringLiteral("/polaris/v1/client-settings");
    script.status = status;
    if (status != 204) {
        script.headers.insert(QByteArrayLiteral("Content-Type"),
                              QByteArrayLiteral("application/json"));
        script.bodyChunks = {QByteArrayLiteral("{}")};
    }
    script.inspectBody = [output, exactBody](QByteArray& body) {
        const QJsonObject object = QJsonDocument::fromJson(body).object();
        *exactBody = object.value(QStringLiteral("output_name")).toString() ==
                output &&
            object.value(QStringLiteral("session_token")).toString() ==
                QStringLiteral("fixture-owner-token");
        return *exactBody;
    };
    return script;
}

QString displayAction(const HostSnapshot& snapshot, const QString& output)
{
    for (auto it = snapshot.actionStates.cbegin();
         it != snapshot.actionStates.cend(); ++it) {
        const QVariantMap value = it.value().value.toMap();
        if (value.value(QStringLiteral("kind")).toString() ==
                QStringLiteral("output") &&
            value.value(QStringLiteral("id")).toString() == output) {
            return it.key();
        }
    }
    return {};
}

DisplaySessionIdentity displayIdentity(quint64 sessionEpoch)
{
    DisplaySessionIdentity identity;
    identity.computerUuid = QStringLiteral("fixture-computer");
    identity.appId = 42;
    identity.appName = QStringLiteral("Desktop");
    identity.sessionEpoch = sessionEpoch;
    return identity;
}

bool waitForPhase(PolarisAdapter& adapter,
                  const SessionTransitionCoordinator& coordinator,
                  DisplayPhase phase, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (coordinator.phase() != phase && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
        QThread::msleep(1);
    }
    adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
    return coordinator.phase() == phase;
}

int displayPostCount(const FakePolarisServer& server)
{
    int count = 0;
    for (const FakePolarisServer::RequestRecord& record :
         server.requestHistory()) {
        if (record.method == QByteArrayLiteral("POST") &&
            record.path == QStringLiteral("/polaris/v1/client-settings")) {
            ++count;
        }
    }
    return count;
}

bool waitForDiscovery(PolarisAdapter& adapter, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!adapter.discoverySnapshot().complete &&
           timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
        QThread::msleep(1);
    }
    adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
    return adapter.discoverySnapshot().complete;
}

bool waitForDiscoveryGeneration(PolarisAdapter& adapter, quint64 generation,
                                int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (adapter.discoverySnapshot().generation < generation &&
           timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
        QThread::msleep(1);
    }
    adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
    const PolarisDiscoverySnapshot snapshot = adapter.discoverySnapshot();
    return snapshot.complete && snapshot.generation >= generation;
}

bool waitForAction(PolarisAdapter& adapter, const bool& complete,
                   int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!complete && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
        QThread::msleep(1);
    }
    adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
    return complete;
}

QMutex capturedLogMutex;
QStringList capturedLog;

void captureLogMessage(QtMsgType, const QMessageLogContext&,
                       const QString& message)
{
    QMutexLocker locker(&capturedLogMutex);
    capturedLog.append(message);
}

class ScopedLogCapture final
{
public:
    ScopedLogCapture()
    {
        QMutexLocker locker(&capturedLogMutex);
        capturedLog.clear();
        locker.unlock();
        m_Previous = qInstallMessageHandler(captureLogMessage);
    }

    ~ScopedLogCapture()
    {
        qInstallMessageHandler(m_Previous);
    }

    QString joined() const
    {
        QMutexLocker locker(&capturedLogMutex);
        return capturedLog.join(QLatin1Char('\n'));
    }

private:
    QtMessageHandler m_Previous = nullptr;
};
}

void PolarisIntegrationTest::harnessListensOnlyOnEphemeralIpv4Loopback()
{
    FakePolarisServer server;
    QVERIFY2(server.start(), qPrintable(server.failureLabel()));
    QCOMPARE(server.address(), QHostAddress(QHostAddress::LocalHost));
    QVERIFY(server.port() != 0);
    QVERIFY(server.stop());
    QVERIFY(!server.isListening());
}

void PolarisIntegrationTest::realQtBackendUsesMutualTlsAndExactPinnedLeaf()
{
    FakePolarisServer server;
    QVERIFY2(server.start(), qPrintable(server.failureLabel()));
    FakePolarisServer::ResponseScript script;
    script.path = QStringLiteral("/polaris/v1/capabilities");
    script.headers.insert(QByteArrayLiteral("Content-Type"),
                          QByteArrayLiteral("application/json"));
    script.bodyChunks = {QByteArrayLiteral("{\"version\":1}")};
    server.enqueue(std::move(script));

    PolarisResponse response;
    {
        PolarisApiClient client(
            pairedComputer(server), PolarisApiClient::TestIdentityTag{},
            server.clientSslConfiguration());
        client.get(QStringLiteral("/polaris/v1/capabilities"), true,
            [&](auto, const PolarisResponse& result) { response = result; });
        QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
        QCOMPARE(client.drainCompletions(), 1);
        QCOMPARE(client.drainCompletions(), 0);
    }

    QVERIFY(response.authenticated);
    QCOMPARE(response.httpStatus, 200);
    QVERIFY(response.errorCode.isEmpty());
    QCOMPARE(response.json.object().value(QStringLiteral("version")).toInt(),
             1);
    QCOMPARE(server.requestHistory().size(), 1);
    QCOMPARE(server.requestHistory().constFirst().method,
             QByteArrayLiteral("GET"));
    QCOMPARE(server.requestHistory().constFirst().path,
             QStringLiteral("/polaris/v1/capabilities"));
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
    QVERIFY(server.stop());
}

void PolarisIntegrationTest::teardownWipesDelayedResponseAndCompletesRequestOnce()
{
    auto server = std::make_unique<FakePolarisServer>();
    QVERIFY2(server->start(), qPrintable(server->failureLabel()));
    const auto responseAudit = server->responseWipeAudit();
    QVERIFY(responseAudit != nullptr);
    const QByteArray delayedBodyFirst(
        "delayed-sensitive-response-sentinel-first");
    const QByteArray delayedBodySecond(
        "delayed-sensitive-response-sentinel-second");
    FakePolarisServer::ResponseScript script;
    script.path = QStringLiteral("/polaris/v1/capabilities");
    script.bodyChunks = {delayedBodyFirst, delayedBodySecond};
    script.delayBeforeHeadersMs = 5000;
    server->enqueue(std::move(script));

    PolarisApiClient client(
        pairedComputer(*server), PolarisApiClient::TestIdentityTag{},
        server->clientSslConfiguration());
    int completionCount = 0;
    PolarisRequestOptions timeout;
    timeout.totalTimeoutMs = 40;
    timeout.idleTimeoutMs = 40;
    client.get(QStringLiteral("/polaris/v1/capabilities"), true,
        [&](auto, const PolarisResponse&) { ++completionCount; }, timeout);
    QVERIFY(server->waitForRequestCount(1, 2000));
    QElapsedTimer timeoutWait;
    timeoutWait.start();
    while (client.pendingCompletionCount() != 1 &&
           timeoutWait.elapsed() < 2000) {
        // Keep the fake server's main thread out of the event loop. The real
        // Polaris worker times out on its own thread, but the server socket's
        // DeferredDelete cannot run before the immediate server destruction.
        QThread::msleep(1);
    }
    QCOMPARE(client.pendingCompletionCount(), 1);
    QCOMPARE(responseAudit->wipeCount, 0);

    QEventLoop disconnectLoop;
    int pendingDeleteCallbackCount = 0;
    server->setPendingDeleteObserver([&] {
        ++pendingDeleteCallbackCount;
        disconnectLoop.quit();
    });
    QTimer::singleShot(2000, &disconnectLoop, &QEventLoop::quit);
    disconnectLoop.exec();
    QCOMPARE(pendingDeleteCallbackCount, 1);
    QCOMPARE(server->pendingDeleteConnectionCount(), qsizetype(1));
    QCOMPARE(server->synchronousTeardownSocketCount(), qsizetype(1));
    QCOMPARE(responseAudit->wipeCount, 0);

    server.reset();
    QCOMPARE(responseAudit->wipeCount, 2);
    QCOMPARE(responseAudit->logicalBytesWiped,
             qsizetype(delayedBodyFirst.size() + delayedBodySecond.size()));
    QVERIFY(responseAudit->allBytesWereZero);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(responseAudit->wipeCount, 2);
    QCOMPARE(responseAudit->logicalBytesWiped,
             qsizetype(delayedBodyFirst.size() + delayedBodySecond.size()));
    QCOMPARE(pendingDeleteCallbackCount, 1);
    QCOMPARE(client.drainCompletions(), 1);
    QCOMPARE(client.drainCompletions(), 0);
    QCOMPARE(completionCount, 1);
}

void PolarisIntegrationTest::authenticatedRawParserRejectsMalformedRequests_data()
{
    QTest::addColumn<QByteArray>("request");
    QTest::addColumn<QString>("failureLabel");

    QTest::newRow("bare-lf")
        << QByteArrayLiteral(
               "GET /polaris/v1/capabilities HTTP/1.1\n"
               "Host: localhost\r\n\r\n")
        << QStringLiteral("invalid_line_ending");
    QTest::newRow("malformed-header-name")
        << QByteArrayLiteral(
               "GET /polaris/v1/capabilities HTTP/1.1\r\n"
               "Host: localhost\r\nBad Name: value\r\n\r\n")
        << QStringLiteral("invalid_header_name");
    QTest::newRow("missing-host")
        << QByteArrayLiteral(
               "GET /polaris/v1/capabilities HTTP/1.1\r\n"
               "User-Agent: parser-test\r\n\r\n")
        << QStringLiteral("missing_host");
    QTest::newRow("empty-host")
        << QByteArrayLiteral(
               "GET /polaris/v1/capabilities HTTP/1.1\r\n"
               "Host:   \r\n\r\n")
        << QStringLiteral("invalid_host");
    QTest::newRow("duplicate-host")
        << QByteArrayLiteral(
               "GET /polaris/v1/capabilities HTTP/1.1\r\n"
               "Host: localhost\r\nHost: localhost\r\n\r\n")
        << QStringLiteral("invalid_host");
    QTest::newRow("request-line-limit-plus-one")
        << QByteArray(4097, 'G')
        << QStringLiteral("request_line_too_large");

    const QByteArray headerPrefix = QByteArrayLiteral(
        "GET /polaris/v1/capabilities HTTP/1.1\r\n"
        "Host: localhost\r\nX-Pad: ");
    const QByteArray headerSuffix = QByteArrayLiteral("\r\n\r\n");
    const qsizetype oversizedHeaderBytes = 64 * 1024 + 1;
    const qsizetype paddingBytes = oversizedHeaderBytes -
        headerPrefix.size() - headerSuffix.size();
    QVERIFY(paddingBytes > 0);
    QTest::newRow("header-limit-plus-one")
        << headerPrefix + QByteArray(paddingBytes, 'h') + headerSuffix
        << QStringLiteral("headers_too_large");
    QTest::newRow("body-limit-plus-one")
        << QByteArrayLiteral(
               "POST /polaris/v1/client-settings HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "Content-Length: 1048577\r\n\r\n")
        << QStringLiteral("invalid_content_length");
}

void PolarisIntegrationTest::authenticatedRawParserRejectsMalformedRequests()
{
    QFETCH(QByteArray, request);
    QFETCH(QString, failureLabel);
    FakePolarisServer server;
    QVERIFY2(server.start(), qPrintable(server.failureLabel()));

    QSslSocket socket;
    socket.setSslConfiguration(server.clientSslConfiguration());
    socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"),
                                  server.port());
    QTRY_VERIFY_WITH_TIMEOUT(socket.isEncrypted(), 2000);
    QCOMPARE(socket.peerCertificate(), server.serverCertificate());
    QCOMPARE(socket.write(request), qint64(request.size()));
    socket.flush();

    QTRY_COMPARE_WITH_TIMEOUT(server.failureLabel(), failureLabel, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(),
                              QAbstractSocket::UnconnectedState, 2000);
    QVERIFY(server.requestHistory().isEmpty());
    QCOMPARE(server.unexpectedRequestCount(), 0);
    QCOMPARE(server.rejectedTlsConnectionCount(), 0);
    QCOMPARE(server.pendingScriptCount(), 0);
    QVERIFY(server.stop());
    QVERIFY(server.requestHistory().isEmpty());
    QVERIFY(server.allRequestBodyWipesWereZero());
}

void PolarisIntegrationTest::currentDiscoveryPublishesPolarisActionsAndDynamicTargets()
{
    FakePolarisServer server;
    QVERIFY2(server.start(), qPrintable(server.failureLabel()));
    enqueueCurrentDiscovery(server);
    AdapterHarness harness(server);

    QVERIFY(harness.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(harness.adapter));
    const PolarisDiscoverySnapshot discovery =
        harness.adapter.discoverySnapshot();
    QVERIFY(discovery.capabilities.valid);
    QVERIFY(discovery.capabilities.isPolarisContract);
    QVERIFY(discovery.session.tokenValid);
    QCOMPARE(discovery.settings.targets.size(), 6);

    const HostSnapshot snapshot = harness.adapter.snapshot();
    QVERIFY(snapshot.actionStates.value(
        QStringLiteral("clipboard.send-local")).enabled);
    QVERIFY(snapshot.actionStates.value(
        QStringLiteral("clipboard.fetch-remote")).enabled);
    QVERIFY(snapshot.actionStates.value(
        QStringLiteral("session.end-host")).enabled);
    QVERIFY(snapshot.actionStates.value(
        QStringLiteral("host.command.2")).enabled);
    QVERIFY(snapshot.actionStates.value(
        QStringLiteral("host.command.0")).disruptive);
    QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
    int dynamicTargets = 0;
    for (auto iterator = snapshot.actionStates.cbegin();
         iterator != snapshot.actionStates.cend(); ++iterator) {
        dynamicTargets += iterator.key().startsWith(
            QStringLiteral("display.target.")) ? 1 : 0;
    }
    QCOMPARE(dynamicTargets, 6);
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
}

void PolarisIntegrationTest::oldMalformedAndNonPolarisDiscoveryFailClosed()
{
    {
        FakePolarisServer server;
        QVERIFY(server.start());
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/capabilities"),
                                  QStringLiteral("capabilities-old.json")));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                                  QStringLiteral("status-owner.json")));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/client-settings"),
                                  QStringLiteral("client-settings-current.json")));
        AdapterHarness harness(server);
        QVERIFY(harness.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(harness.adapter));
        const HostSnapshot snapshot = harness.adapter.snapshot();
        QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
        QVERIFY(!snapshot.actionStates.value(
            QStringLiteral("clipboard.send-local")).enabled);
        QVERIFY(!snapshot.actionStates.value(
            QStringLiteral("session.end-host")).enabled);
        QVERIFY(!snapshot.actionStates.value(
            QStringLiteral("display.switch")).enabled);
        QCOMPARE(snapshot.actionStates.value(
            QStringLiteral("display.switch")).disabledCode,
            QStringLiteral("capability_not_advertised"));
    }

    {
        FakePolarisServer server;
        QVERIFY(server.start());
        auto malformed = jsonScript(
            QStringLiteral("/polaris/v1/capabilities"),
            QStringLiteral("capabilities-current.json"));
        malformed.bodyChunks = {QByteArrayLiteral("{not-json")};
        server.enqueue(std::move(malformed));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                                  QStringLiteral("status-owner.json")));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/client-settings"),
                                  QStringLiteral("client-settings-current.json")));
        AdapterHarness harness(server);
        QVERIFY(harness.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(harness.adapter));
        const HostSnapshot snapshot = harness.adapter.snapshot();
        QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
        QVERIFY(!snapshot.actionStates.value(
            QStringLiteral("clipboard.send-local")).enabled);
        QVERIFY(!snapshot.actionStates.value(
            QStringLiteral("display.switch")).enabled);
    }

    {
        FakePolarisServer server;
        QVERIFY(server.start());
        auto notFound = jsonScript(
            QStringLiteral("/polaris/v1/capabilities"),
            QStringLiteral("capabilities-current.json"), 404);
        server.enqueue(std::move(notFound));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                                  QStringLiteral("status-owner.json")));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/client-settings"),
                                  QStringLiteral("client-settings-current.json")));
        AdapterHarness harness(server);
        QVERIFY(harness.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(harness.adapter));
        const PolarisDiscoverySnapshot discovery =
            harness.adapter.discoverySnapshot();
        QVERIFY(discovery.standardHost);
        const HostSnapshot snapshot = harness.adapter.snapshot();
        QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
        for (auto iterator = snapshot.actionStates.cbegin();
             iterator != snapshot.actionStates.cend(); ++iterator) {
            QVERIFY(!iterator.key().startsWith(
                QStringLiteral("display.target.")));
        }
    }
}

void PolarisIntegrationTest::wrongServerLeafAndClientIdentityFailClosed()
{
    {
        FakePolarisServer server;
        QVERIFY(server.start());
        auto script = jsonScript(QStringLiteral("/polaris/v1/capabilities"),
                                 QStringLiteral("capabilities-current.json"));
        server.enqueue(std::move(script));
        NvComputer computer = pairedComputer(server);
        computer.serverCert = server.clientCertificate();
        PolarisResponse response;
        {
            PolarisApiClient client(
                computer, PolarisApiClient::TestIdentityTag{},
                server.clientSslConfiguration());
            client.get(QStringLiteral("/polaris/v1/capabilities"), true,
                [&](auto, const PolarisResponse& result) { response = result; });
            QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
            QCOMPARE(client.drainCompletions(), 1);
            QCOMPARE(client.drainCompletions(), 0);
        }
        QVERIFY(!response.authenticated);
        QCOMPARE(response.errorCode, QStringLiteral("tls_identity_mismatch"));
        QVERIFY(server.requestHistory().isEmpty());
    }

    for (bool useWrongIdentity : {false, true}) {
        FakePolarisServer server;
        QVERIFY(server.start());
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/capabilities"),
                                  QStringLiteral("capabilities-current.json")));
        PolarisResponse response;
        {
            const QSslConfiguration identity = useWrongIdentity
                ? server.wrongClientSslConfiguration()
                : server.emptyClientSslConfiguration();
            PolarisApiClient client(
                pairedComputer(server), PolarisApiClient::TestIdentityTag{},
                identity);
            client.get(QStringLiteral("/polaris/v1/capabilities"), true,
                [&](auto, const PolarisResponse& result) { response = result; });
            QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
            QCOMPARE(client.drainCompletions(), 1);
            QCOMPARE(client.drainCompletions(), 0);
        }
        QVERIFY(response.errorCode == QStringLiteral("tls_identity_mismatch") ||
                response.errorCode == QStringLiteral("network_error"));
        QVERIFY(server.requestHistory().isEmpty());
        QVERIFY(server.rejectedTlsConnectionCount() > 0 ||
                server.failureLabel() == QStringLiteral("client_tls_rejected"));
    }
}

void PolarisIntegrationTest::authenticatedHttpTimeoutDropAndPolicyRemainDistinct()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    const QList<int> statuses = {401, 403, 409, 470, 500};
    for (int status : statuses) {
        FakePolarisServer::ResponseScript script;
        script.method = QByteArrayLiteral("POST");
        script.path = QStringLiteral("/polaris/v1/session/stop");
        script.status = status;
        script.bodyChunks = {QByteArrayLiteral("{}")};
        server.enqueue(std::move(script));
    }
    FakePolarisServer::ResponseScript timeout;
    timeout.path = QStringLiteral("/polaris/v1/timeout");
    timeout.stall = true;
    server.enqueue(std::move(timeout));
    FakePolarisServer::ResponseScript drop;
    drop.path = QStringLiteral("/polaris/v1/drop");
    drop.disconnectBeforeHeaders = true;
    server.enqueue(drop);
    // Qt retries one idempotent GET if the peer closes before any response
    // bytes arrive. Both attempts remain exact scripted loopback requests,
    // while PolarisApiClient still publishes one terminal completion.
    server.enqueue(std::move(drop));

    QList<PolarisResponse> responses;
    {
        PolarisApiClient client(
            pairedComputer(server), PolarisApiClient::TestIdentityTag{},
            server.clientSslConfiguration());
        for (int ignored : statuses) {
            Q_UNUSED(ignored);
            client.request(QByteArrayLiteral("POST"),
                QStringLiteral("/polaris/v1/session/stop"),
                QByteArrayLiteral("{}"), false,
                [&](auto, const PolarisResponse& response) {
                    responses.append(response);
                });
            QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
            QCOMPARE(client.drainCompletions(1), 1);
        }
        PolarisRequestOptions shortTimeout;
        shortTimeout.totalTimeoutMs = 80;
        shortTimeout.idleTimeoutMs = 80;
        client.get(QStringLiteral("/polaris/v1/timeout"), false,
            [&](auto, const PolarisResponse& response) {
                responses.append(response);
            }, shortTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
        QCOMPARE(client.drainCompletions(1), 1);
        client.get(QStringLiteral("/polaris/v1/drop"), false,
            [&](auto, const PolarisResponse& response) {
                responses.append(response);
            });
        QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
        QCOMPARE(client.drainCompletions(1), 1);
        client.get(QStringLiteral("https://escape.invalid/polaris/v1/status"),
            false, [&](auto, const PolarisResponse& response) {
                responses.append(response);
            });
        QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 2000);
        QCOMPARE(client.drainCompletions(1), 1);
        QCOMPARE(client.drainCompletions(16), 0);
        QTest::qWait(20);
        QCOMPARE(client.drainCompletions(16), 0);
    }

    QCOMPARE(responses.size(), 8);
    for (int status : statuses) {
        const auto match = std::find_if(
            responses.cbegin(), responses.cend(),
            [status](const PolarisResponse& response) {
                return response.httpStatus == status;
            });
        QVERIFY(match != responses.cend());
        QVERIFY(match->authenticated);
        QCOMPARE(match->errorCode, QStringLiteral("http_error"));
    }
    QStringList errorCodes;
    for (const PolarisResponse& response : std::as_const(responses)) {
        errorCodes.append(response.errorCode);
    }
    QVERIFY(errorCodes.contains(QStringLiteral("timeout")));
    QVERIFY(errorCodes.contains(QStringLiteral("network_error")));
    QVERIFY(errorCodes.contains(QStringLiteral("endpoint_policy_rejected")));
    QCOMPARE(server.requestHistory().size(), 8);
    QCOMPARE(server.unexpectedRequestCount(), 0);
}

void PolarisIntegrationTest::partialUnknownDiscoveryAndClipboardPermissionsStayIndependent()
{
    const auto runPermissionCase = [](bool readAllowed, bool writeAllowed,
                                      bool splitResponses) {
        FakePolarisServer server;
        QVERIFY(server.start());

        QJsonObject capabilities = QJsonDocument::fromJson(
            fixture(QStringLiteral("capabilities-current.json"))).object();
        capabilities.insert(QStringLiteral("future_top_level"),
                            QJsonObject{{QStringLiteral("ignored"), true}});
        QByteArray capabilityBody = QJsonDocument(capabilities).toJson(
            QJsonDocument::Compact);
        FakePolarisServer::ResponseScript capabilityScript = jsonBodyScript(
            QStringLiteral("/polaris/v1/capabilities"), {});
        if (splitResponses) {
            const qsizetype split = capabilityBody.size() / 2;
            capabilityScript.bodyChunks = {
                capabilityBody.left(split), capabilityBody.mid(split)};
            capabilityScript.delayBetweenChunksMs = 5;
        }
        else {
            capabilityScript.bodyChunks = {std::move(capabilityBody)};
        }
        server.enqueue(std::move(capabilityScript));

        QJsonObject status = QJsonDocument::fromJson(
            fixture(QStringLiteral("status-owner.json"))).object();
        QJsonObject controls = status.value(
            QStringLiteral("controls")).toObject();
        controls.insert(QStringLiteral("clipboard_read_allowed"), readAllowed);
        controls.insert(QStringLiteral("clipboard_write_allowed"), writeAllowed);
        controls.insert(QStringLiteral("future_permission"),
                        QStringLiteral("ignored"));
        status.insert(QStringLiteral("controls"), controls);
        server.enqueue(jsonBodyScript(
            QStringLiteral("/polaris/v1/session/status"),
            QJsonDocument(status).toJson(QJsonDocument::Compact)));

        QJsonObject settings = QJsonDocument::fromJson(
            fixture(QStringLiteral("client-settings-current.json"))).object();
        settings.insert(QStringLiteral("future_settings"),
                        QJsonArray{1, QStringLiteral("ignored")});
        QByteArray settingsBody = QJsonDocument(settings).toJson(
            QJsonDocument::Compact);
        FakePolarisServer::ResponseScript settingsScript = jsonBodyScript(
            QStringLiteral("/polaris/v1/client-settings"), {});
        if (splitResponses) {
            const qsizetype split = settingsBody.size() / 3;
            settingsScript.bodyChunks = {
                settingsBody.left(split), settingsBody.mid(split)};
            settingsScript.delayBetweenChunksMs = 5;
        }
        else {
            settingsScript.bodyChunks = {std::move(settingsBody)};
        }
        server.enqueue(std::move(settingsScript));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/commands"),
                                  QStringLiteral("commands-current.json")));

        AdapterHarness harness(server);
        QVERIFY(harness.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(harness.adapter));
        const PolarisDiscoverySnapshot discovery =
            harness.adapter.discoverySnapshot();
        QVERIFY(discovery.capabilities.valid);
        QVERIFY(discovery.settings.valid);
        QCOMPARE(discovery.settings.targets.size(), 6);
        const HostSnapshot snapshot = harness.adapter.snapshot();
        QCOMPARE(snapshot.actionStates.value(
            QStringLiteral("clipboard.fetch-remote")).enabled, readAllowed);
        QCOMPARE(snapshot.actionStates.value(
            QStringLiteral("clipboard.send-local")).enabled, writeAllowed);
        if (!readAllowed) {
            QCOMPARE(snapshot.actionStates.value(
                QStringLiteral("clipboard.fetch-remote")).disabledCode,
                QStringLiteral("permission_denied"));
        }
        if (!writeAllowed) {
            QCOMPARE(snapshot.actionStates.value(
                QStringLiteral("clipboard.send-local")).disabledCode,
                QStringLiteral("permission_denied"));
        }
        QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
        QVERIFY(snapshot.actionStates.value(QStringLiteral("host.command.2")).enabled);
        QCOMPARE(server.pendingScriptCount(), 0);
        QCOMPARE(server.unexpectedRequestCount(), 0);
    };

    runPermissionCase(true, false, true);
    runPermissionCase(false, true, false);

    {
        FakePolarisServer server;
        QVERIFY(server.start());
        QJsonObject capabilities = QJsonDocument::fromJson(
            fixture(QStringLiteral("capabilities-current.json"))).object();
        QJsonObject features = capabilities.value(
            QStringLiteral("features")).toObject();
        features.remove(QStringLiteral("named_commands_v1"));
        capabilities.insert(QStringLiteral("features"), features);
        capabilities.remove(QStringLiteral("named_commands"));
        server.enqueue(jsonBodyScript(
            QStringLiteral("/polaris/v1/capabilities"),
            QJsonDocument(capabilities).toJson(QJsonDocument::Compact)));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                                  QStringLiteral("status-owner.json")));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/client-settings"),
                                  QStringLiteral("client-settings-current.json")));
        AdapterHarness harness(server);
        QVERIFY(harness.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(harness.adapter));
        const HostSnapshot snapshot = harness.adapter.snapshot();
        QVERIFY(!snapshot.actionStates.value(
            QStringLiteral("host.command")).enabled);
        QCOMPARE(snapshot.actionStates.value(
            QStringLiteral("host.command")).disabledCode,
            QStringLiteral("capability_not_advertised"));
        QVERIFY(snapshot.actionStates.value(
            QStringLiteral("clipboard.send-local")).enabled);
        QVERIFY(snapshot.actionStates.value(
            QStringLiteral("session.end-host")).enabled);
        QVERIFY(snapshot.actionStates.value(
            QStringLiteral("display.switch")).enabled);
        QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
        QCOMPARE(server.requestHistory().size(), 3);
        QCOMPARE(server.pendingScriptCount(), 0);
    }

    {
        FakePolarisServer server;
        QVERIFY(server.start());
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/capabilities"),
                                  QStringLiteral("capabilities-current.json")));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/session/status"),
                                  QStringLiteral("status-owner.json")));
        QJsonObject settings = QJsonDocument::fromJson(
            fixture(QStringLiteral("client-settings-current.json"))).object();
        QJsonObject settingsCapabilities = settings.value(
            QStringLiteral("capabilities")).toObject();
        settingsCapabilities.remove(QStringLiteral("outputs"));
        settings.insert(QStringLiteral("capabilities"), settingsCapabilities);
        server.enqueue(jsonBodyScript(
            QStringLiteral("/polaris/v1/client-settings"),
            QJsonDocument(settings).toJson(QJsonDocument::Compact)));
        server.enqueue(jsonScript(QStringLiteral("/polaris/v1/commands"),
                                  QStringLiteral("commands-current.json")));
        AdapterHarness harness(server);
        QVERIFY(harness.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(harness.adapter));
        const HostSnapshot snapshot = harness.adapter.snapshot();
        bool foundMode = false;
        bool foundOutput = false;
        for (auto it = snapshot.actionStates.cbegin();
             it != snapshot.actionStates.cend(); ++it) {
            const QVariantMap metadata = it.value().value.toMap();
            foundMode |= metadata.value(QStringLiteral("kind")).toString() ==
                QStringLiteral("stream-mode");
            foundOutput |= metadata.value(QStringLiteral("kind")).toString() ==
                QStringLiteral("output");
        }
        QVERIFY(foundMode);
        QVERIFY(!foundOutput);
        QVERIFY(snapshot.actionStates.value(
            QStringLiteral("clipboard.fetch-remote")).enabled);
        QVERIFY(snapshot.actionStates.value(
            QStringLiteral("host.command.2")).enabled);
        QVERIFY(snapshot.actionStates.value(
            QStringLiteral("session.end-host")).enabled);
        QVERIFY(snapshot.actionStates.value(QStringLiteral("stats.overlay")).enabled);
        QCOMPARE(server.pendingScriptCount(), 0);
        QCOMPARE(server.unexpectedRequestCount(), 0);
    }
}

void PolarisIntegrationTest::publicActionsMapAuthenticatedFailuresExactly()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    enqueueCurrentDiscovery(server);
    AdapterHarness harness(server);
    QVERIFY(harness.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(harness.adapter));

    const QList<QPair<int, QString>> expected{
        {401, QStringLiteral("authentication_failed")},
        {403, QStringLiteral("permission_denied")},
        {409, QStringLiteral("stale_session")},
        {470, QStringLiteral("session_not_owned")},
        {500, QStringLiteral("host_operation_failed")},
    };
    for (const auto& pair : expected) {
        bool exactBody = false;
        FakePolarisServer::ResponseScript script;
        script.method = QByteArrayLiteral("POST");
        script.path = QStringLiteral("/polaris/v1/session/stop");
        script.status = pair.first;
        script.headers.insert(QByteArrayLiteral("Content-Type"),
                              QByteArrayLiteral("application/json"));
        script.bodyChunks = {QByteArrayLiteral("{}")};
        script.inspectBody = [&](QByteArray& body) {
            const QJsonObject object = QJsonDocument::fromJson(body).object();
            exactBody = object.size() == 1 &&
                object.value(QStringLiteral("session_token")).toString() ==
                    QStringLiteral("fixture-owner-token");
            return exactBody;
        };
        server.enqueue(std::move(script));

        QVERIFY(harness.registry.state(
            QStringLiteral("session.end-host")).enabled);
        QVERIFY(harness.registry.beginConfirmation(
            QStringLiteral("session.end-host")));
        ActionResult result;
        bool complete = false;
        harness.registry.acceptConfirmation(
            QStringLiteral("session.end-host"), {},
            [&](const ActionResult& value) {
                result = value;
                complete = true;
            });
        QVERIFY(waitForAction(harness.adapter, complete));
        QVERIFY(exactBody);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, pair.second);
    }
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
    QVERIFY(server.allRequestBodyWipesWereZero());
}

void PolarisIntegrationTest::namedCommandsUseMetadataConfirmationAndAuthenticatedAcks()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    enqueueCurrentDiscovery(server);
    AdapterHarness harness(server);
    ScopedLogCapture logCapture;
    QVERIFY(harness.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(harness.adapter));

    const ActionState safe = harness.registry.state(
        QStringLiteral("host.command.2"));
    const ActionState disruptive = harness.registry.state(
        QStringLiteral("host.command.0"));
    QVERIFY(safe.enabled);
    QVERIFY(!safe.disruptive);
    QVERIFY(disruptive.enabled);
    QVERIFY(disruptive.disruptive);
    QVERIFY(!harness.registry.requiresConfirmation(
        QStringLiteral("host.command.2")));
    QVERIFY(harness.registry.requiresConfirmation(
        QStringLiteral("host.command.0")));
    QVERIFY(!safe.value.toMap().contains(QStringLiteral("command")));
    QVERIFY(!disruptive.value.toMap().contains(QStringLiteral("command")));

    const auto enqueueCommand = [&](int index, int status,
                                    bool* exactBody) {
        FakePolarisServer::ResponseScript script;
        script.method = QByteArrayLiteral("POST");
        script.path = QStringLiteral("/polaris/v1/commands");
        script.status = status;
        script.headers.insert(QByteArrayLiteral("Content-Type"),
                              QByteArrayLiteral("application/json"));
        script.bodyChunks = status == 202
            ? QList<QByteArray>{QByteArrayLiteral(
                  "{\"accepted\":true,\"state\":\"accepted\"}")}
            : QList<QByteArray>{QByteArrayLiteral("{}")};
        script.inspectBody = [=](QByteArray& body) {
            const QJsonObject object = QJsonDocument::fromJson(body).object();
            *exactBody = object.size() == 2 &&
                object.value(QStringLiteral("index")).toInt(-1) == index &&
                object.value(QStringLiteral("session_token")).toString() ==
                    QStringLiteral("fixture-owner-token");
            return *exactBody;
        };
        server.enqueue(std::move(script));
    };
    const auto execute = [&](const QString& actionId, bool confirm,
                             ActionResult* result) {
        bool complete = false;
        if (confirm) {
            QVERIFY(harness.registry.state(actionId).enabled);
            QVERIFY(harness.registry.beginConfirmation(actionId));
            harness.registry.acceptConfirmation(
                actionId, {}, [&](const ActionResult& value) {
                    *result = value;
                    complete = true;
                });
        }
        else {
            harness.registry.execute(
                actionId, {}, [&](const ActionResult& value) {
                    *result = value;
                    complete = true;
                });
        }
        QVERIFY(waitForAction(harness.adapter, complete));
    };

    bool safeBody = false;
    enqueueCommand(2, 202, &safeBody);
    enqueueCurrentDiscovery(server);
    ActionResult safeResult;
    execute(QStringLiteral("host.command.2"), false, &safeResult);
    QVERIFY(safeBody);
    QVERIFY(safeResult.ok);
    QVERIFY(waitForDiscoveryGeneration(harness.adapter, 2));

    const int requestsBeforeUnconfirmed = server.requestHistory().size();
    ActionResult confirmationRequired;
    execute(QStringLiteral("host.command.0"), false, &confirmationRequired);
    QCOMPARE(confirmationRequired.errorCode,
             QStringLiteral("confirmation_required"));
    QCOMPARE(server.requestHistory().size(), requestsBeforeUnconfirmed);

    bool disruptiveBody = false;
    enqueueCommand(0, 202, &disruptiveBody);
    enqueueCurrentDiscovery(server);
    ActionResult disruptiveResult;
    execute(QStringLiteral("host.command.0"), true, &disruptiveResult);
    QVERIFY(disruptiveBody);
    QVERIFY(disruptiveResult.ok);
    QVERIFY(waitForDiscoveryGeneration(harness.adapter, 3));

    for (const auto& pair : QList<QPair<int, QString>>{
             {403, QStringLiteral("permission_denied")},
             {409, QStringLiteral("stale_session")}}) {
        bool exactBody = false;
        enqueueCommand(2, pair.first, &exactBody);
        ActionResult result;
        execute(QStringLiteral("host.command.2"), false, &result);
        QVERIFY(exactBody);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, pair.second);
    }

    const QString logs = logCapture.joined();
    QVERIFY(!logs.contains(QStringLiteral("fixture-owner-token")));
    QVERIFY(!logs.contains(QStringLiteral(
        "DO_NOT_RETAIN_RAW_COMMAND_SECRET")));
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
    QVERIFY(server.allRequestBodyWipesWereZero());
}

void PolarisIntegrationTest::inPlaceDisplaySwitchRequiresFreshReadbackAndFrame()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    SessionTransitionCoordinator coordinator;
    AdapterHarness harness(server, &coordinator);
    auto port = std::make_shared<IntegrationDisplayPort>(&harness.adapter);
    QVERIFY(coordinator.attachSession(displayIdentity(1), port));
    enqueueDiscoveryWithSettings(
        server, displaySettings(QStringLiteral("DP-1"), false));
    QVERIFY(harness.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(harness.adapter));
    const QString actionId = displayAction(
        harness.adapter.snapshot(), QStringLiteral("HDMI-A-1"));
    QVERIFY(!actionId.isEmpty());
    QVERIFY(harness.registry.state(actionId).enabled);

    bool exactPost = false;
    server.enqueue(displayPost(QStringLiteral("HDMI-A-1"), &exactPost));
    enqueueDiscoveryWithSettings(
        server, displaySettings(QStringLiteral("HDMI-A-1"), false));
    ActionResult result;
    bool complete = false;
    harness.registry.execute(actionId, {}, [&](const ActionResult& value) {
        result = value;
        complete = true;
    });
    QVERIFY(waitForDiscoveryGeneration(harness.adapter, 2));
    QVERIFY(exactPost);
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QVERIFY(!complete);
    QCOMPARE(port->evidenceEpochs.size(), 1);
    coordinator.observeDiscovery(1, harness.adapter.discoverySnapshot());
    QVERIFY(!complete);
    coordinator.firstFrameDecoded(1, port->evidenceEpochs.constLast());
    QVERIFY(complete);
    QVERIFY(result.ok);
    QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
    QCOMPARE(displayPostCount(server), 1);
    QVERIFY(port->disconnectEpochs.isEmpty());
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
}

void PolarisIntegrationTest::displayDisagreementRollsBackExactlyOnce()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    SessionTransitionCoordinator coordinator;
    AdapterHarness harness(server, &coordinator);
    auto port = std::make_shared<IntegrationDisplayPort>(&harness.adapter);
    QVERIFY(coordinator.attachSession(displayIdentity(2), port));
    const QByteArray previousSettings = displaySettings(
        QStringLiteral("DP-1"), false);
    enqueueDiscoveryWithSettings(server, previousSettings);
    QVERIFY(harness.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(harness.adapter));
    const QString actionId = displayAction(
        harness.adapter.snapshot(), QStringLiteral("HDMI-A-1"));
    QVERIFY(!actionId.isEmpty());
    QVERIFY(harness.registry.state(actionId).enabled);

    bool requestedPost = false;
    bool rollbackPost = false;
    server.enqueue(displayPost(QStringLiteral("HDMI-A-1"), &requestedPost));
    enqueueDiscoveryWithSettings(server, previousSettings);
    server.enqueue(displayPost(QStringLiteral("DP-1"), &rollbackPost));
    enqueueDiscoveryWithSettings(server, previousSettings);
    ActionResult result;
    bool complete = false;
    harness.registry.execute(actionId, {}, [&](const ActionResult& value) {
        result = value;
        complete = true;
    });
    QVERIFY(waitForDiscoveryGeneration(harness.adapter, 2));
    QVERIFY(requestedPost);
    coordinator.observeDiscovery(2, harness.adapter.discoverySnapshot());
    QCOMPARE(coordinator.phase(), DisplayPhase::RollingBack);
    QVERIFY(waitForDiscoveryGeneration(harness.adapter, 3));
    QVERIFY(rollbackPost);
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QCOMPARE(port->evidenceEpochs.size(), 2);
    coordinator.observeDiscovery(2, harness.adapter.discoverySnapshot());
    coordinator.firstFrameDecoded(2, port->evidenceEpochs.constLast());
    QVERIFY(complete);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("switch_failed_restored"));
    QCOMPARE(coordinator.phase(), DisplayPhase::Failed);
    QVERIFY(!coordinator.recoveryVisible());
    QCOMPARE(displayPostCount(server), 2);
    QVERIFY(port->disconnectEpochs.isEmpty());
    QTest::qWait(20);
    harness.adapter.pumpCompletions(PolarisAdapter::CompletionPumpLimit);
    QCOMPARE(displayPostCount(server), 2);
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
}

void PolarisIntegrationTest::reconnectDisplaySwitchSucceedsWithoutStoppingHost()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    SessionTransitionCoordinator coordinator;
    AdapterHarness initial(server, &coordinator);
    auto initialPort = std::make_shared<IntegrationDisplayPort>(
        &initial.adapter);
    QVERIFY(coordinator.attachSession(displayIdentity(10), initialPort));
    enqueueDiscoveryWithSettings(
        server, displaySettings(QStringLiteral("DP-1"), true));
    QVERIFY(initial.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(initial.adapter));
    const QString actionId = displayAction(
        initial.adapter.snapshot(), QStringLiteral("HDMI-A-1"));
    QVERIFY(!actionId.isEmpty());
    QVERIFY(initial.registry.state(actionId).enabled);

    bool exactPost = false;
    server.enqueue(displayPost(QStringLiteral("HDMI-A-1"), &exactPost));
    ActionResult result;
    bool complete = false;
    initial.registry.execute(actionId, {}, [&](const ActionResult& value) {
        result = value;
        complete = true;
    });
    QVERIFY(waitForPhase(initial.adapter, coordinator,
                         DisplayPhase::Disconnecting));
    QVERIFY(exactPost);
    QVERIFY(!complete);
    QCOMPARE(initialPort->disconnectEpochs.size(), 1);
    QVERIFY(coordinator.sessionFinished(10));
    coordinator.detachSession(10);
    QCOMPARE(coordinator.phase(), DisplayPhase::Reconnecting);

    AdapterHarness replacement(server, &coordinator);
    auto replacementPort = std::make_shared<IntegrationDisplayPort>(
        &replacement.adapter);
    enqueueDiscoveryWithSettings(
        server, displaySettings(QStringLiteral("HDMI-A-1"), true));
    QVERIFY(coordinator.attachSession(displayIdentity(11), replacementPort));
    QVERIFY(waitForDiscoveryGeneration(replacement.adapter, 1));
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QCOMPARE(replacementPort->evidenceEpochs.size(), 1);
    coordinator.observeDiscovery(11,
                                 replacement.adapter.discoverySnapshot());
    QVERIFY(!complete);
    coordinator.firstFrameDecoded(
        11, replacementPort->evidenceEpochs.constLast());
    QVERIFY(complete);
    QVERIFY(result.ok);
    QCOMPARE(coordinator.phase(), DisplayPhase::Succeeded);
    QCOMPARE(displayPostCount(server), 1);
    for (const FakePolarisServer::RequestRecord& record :
         server.requestHistory()) {
        QVERIFY(record.path != QStringLiteral("/polaris/v1/session/stop"));
    }
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
}

void PolarisIntegrationTest::reconnectVerificationDeadlineRollsBackExactlyOnce()
{
    FakePolarisServer server;
    QVERIFY(server.start());
    qint64 now = 1000;
    SessionTransitionCoordinator coordinator([&now] { return now; });
    AdapterHarness initial(server, &coordinator);
    auto initialPort = std::make_shared<IntegrationDisplayPort>(
        &initial.adapter);
    QVERIFY(coordinator.attachSession(displayIdentity(40), initialPort));
    enqueueDiscoveryWithSettings(
        server, displaySettings(QStringLiteral("DP-1"), true));
    QVERIFY(initial.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(initial.adapter));
    const QString actionId = displayAction(
        initial.adapter.snapshot(), QStringLiteral("HDMI-A-1"));
    QVERIFY(!actionId.isEmpty());
    QVERIFY(initial.registry.state(actionId).enabled);

    bool requestedPost = false;
    server.enqueue(displayPost(QStringLiteral("HDMI-A-1"),
                               &requestedPost));
    ActionResult result;
    bool complete = false;
    initial.registry.execute(actionId, {}, [&](const ActionResult& value) {
        result = value;
        complete = true;
    });
    QVERIFY(waitForPhase(initial.adapter, coordinator,
                         DisplayPhase::Disconnecting));
    QVERIFY(requestedPost);
    QCOMPARE(initialPort->disconnectEpochs.size(), 1);
    QVERIFY(coordinator.sessionFinished(40));
    coordinator.detachSession(40);

    AdapterHarness timedOutReplacement(server, &coordinator);
    auto timedOutPort = std::make_shared<IntegrationDisplayPort>(
        &timedOutReplacement.adapter, false);
    QVERIFY(coordinator.attachSession(displayIdentity(41), timedOutPort));
    QCOMPARE(coordinator.phase(), DisplayPhase::Verifying);
    QVERIFY(coordinator.verificationPending());
    QVERIFY(!complete);

    bool rollbackPost = false;
    server.enqueue(displayPost(QStringLiteral("DP-1"), &rollbackPost));
    now += 15001;
    coordinator.checkDeadline();
    QVERIFY(waitForPhase(timedOutReplacement.adapter, coordinator,
                         DisplayPhase::Disconnecting));
    QVERIFY(rollbackPost);
    QCOMPARE(displayPostCount(server), 2);
    QCOMPARE(timedOutPort->disconnectEpochs.size(), 1);
    QVERIFY(coordinator.sessionFinished(41));
    coordinator.detachSession(41);
    QCOMPARE(coordinator.phase(), DisplayPhase::Reconnecting);

    AdapterHarness restored(server, &coordinator);
    auto restoredPort = std::make_shared<IntegrationDisplayPort>(
        &restored.adapter);
    enqueueDiscoveryWithSettings(
        server, displaySettings(QStringLiteral("DP-1"), true));
    QVERIFY(coordinator.attachSession(displayIdentity(42), restoredPort));
    QVERIFY(waitForDiscoveryGeneration(restored.adapter, 1));
    coordinator.observeDiscovery(42, restored.adapter.discoverySnapshot());
    coordinator.firstFrameDecoded(
        42, restoredPort->evidenceEpochs.constLast());
    QVERIFY(complete);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("switch_failed_restored"));
    QVERIFY(!coordinator.recoveryVisible());
    QCOMPARE(displayPostCount(server), 2);
    QCOMPARE(restoredPort->disconnectEpochs.size(), 0);

    now += 60000;
    for (int iteration = 0; iteration < 3; ++iteration) {
        coordinator.checkDeadline();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        restored.adapter.pumpCompletions(
            PolarisAdapter::CompletionPumpLimit);
    }
    QCOMPARE(displayPostCount(server), 2);
    QCOMPARE(restoredPort->disconnectEpochs.size(), 0);
    QCOMPARE(server.pendingScriptCount(), 0);
    QCOMPARE(server.unexpectedRequestCount(), 0);
}

void PolarisIntegrationTest::reconnectFailureRollsBackAndRollbackFailureExposesRecovery()
{
    {
        FakePolarisServer server;
        QVERIFY(server.start());
        SessionTransitionCoordinator coordinator;
        AdapterHarness initial(server, &coordinator);
        auto initialPort = std::make_shared<IntegrationDisplayPort>(
            &initial.adapter);
        QVERIFY(coordinator.attachSession(displayIdentity(20), initialPort));
        enqueueDiscoveryWithSettings(
            server, displaySettings(QStringLiteral("DP-1"), true));
        QVERIFY(initial.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(initial.adapter));
        const QString actionId = displayAction(
            initial.adapter.snapshot(), QStringLiteral("HDMI-A-1"));
        QVERIFY(!actionId.isEmpty());
        QVERIFY(initial.registry.state(actionId).enabled);

        bool requestedPost = false;
        server.enqueue(displayPost(QStringLiteral("HDMI-A-1"),
                                   &requestedPost));
        ActionResult result;
        bool complete = false;
        initial.registry.execute(actionId, {}, [&](const ActionResult& value) {
            result = value;
            complete = true;
        });
        QVERIFY(waitForPhase(initial.adapter, coordinator,
                             DisplayPhase::Disconnecting));
        QVERIFY(requestedPost);
        QVERIFY(coordinator.sessionFinished(20));
        coordinator.detachSession(20);

        AdapterHarness failedReplacement(server, &coordinator);
        auto failedPort = std::make_shared<IntegrationDisplayPort>(
            &failedReplacement.adapter, false);
        QVERIFY(coordinator.attachSession(displayIdentity(21), failedPort));
        bool rollbackPost = false;
        server.enqueue(displayPost(QStringLiteral("DP-1"), &rollbackPost));
        coordinator.sessionConnectionFailed(
            21, QStringLiteral("connection_failed"));
        QVERIFY(waitForPhase(failedReplacement.adapter, coordinator,
                             DisplayPhase::Disconnecting));
        QVERIFY(rollbackPost);
        QCOMPARE(displayPostCount(server), 2);
        QCOMPARE(failedPort->disconnectEpochs.size(), 1);
        QVERIFY(coordinator.sessionFinished(21));
        coordinator.detachSession(21);

        AdapterHarness restored(server, &coordinator);
        auto restoredPort = std::make_shared<IntegrationDisplayPort>(
            &restored.adapter);
        enqueueDiscoveryWithSettings(
            server, displaySettings(QStringLiteral("DP-1"), true));
        QVERIFY(coordinator.attachSession(displayIdentity(22), restoredPort));
        QVERIFY(waitForDiscoveryGeneration(restored.adapter, 1));
        coordinator.observeDiscovery(22,
                                     restored.adapter.discoverySnapshot());
        coordinator.firstFrameDecoded(
            22, restoredPort->evidenceEpochs.constLast());
        QVERIFY(complete);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode,
                 QStringLiteral("switch_failed_restored"));
        QVERIFY(!coordinator.recoveryVisible());
        QCOMPARE(displayPostCount(server), 2);
        QTest::qWait(20);
        restored.adapter.pumpCompletions(
            PolarisAdapter::CompletionPumpLimit);
        QCOMPARE(displayPostCount(server), 2);
        QCOMPARE(server.pendingScriptCount(), 0);
        QCOMPARE(server.unexpectedRequestCount(), 0);
    }

    {
        FakePolarisServer server;
        QVERIFY(server.start());
        SessionTransitionCoordinator coordinator;
        AdapterHarness initial(server, &coordinator);
        auto initialPort = std::make_shared<IntegrationDisplayPort>(
            &initial.adapter);
        QVERIFY(coordinator.attachSession(displayIdentity(30), initialPort));
        enqueueDiscoveryWithSettings(
            server, displaySettings(QStringLiteral("DP-1"), true));
        QVERIFY(initial.adapter.startDiscovery());
        QVERIFY(waitForDiscovery(initial.adapter));
        const QString actionId = displayAction(
            initial.adapter.snapshot(), QStringLiteral("HDMI-A-1"));
        QVERIFY(!actionId.isEmpty());
        QVERIFY(initial.registry.state(actionId).enabled);

        bool requestedPost = false;
        server.enqueue(displayPost(QStringLiteral("HDMI-A-1"),
                                   &requestedPost));
        ActionResult result;
        bool complete = false;
        initial.registry.execute(actionId, {}, [&](const ActionResult& value) {
            result = value;
            complete = true;
        });
        QVERIFY(waitForPhase(initial.adapter, coordinator,
                             DisplayPhase::Disconnecting));
        QVERIFY(requestedPost);
        QVERIFY(coordinator.sessionFinished(30));
        coordinator.detachSession(30);

        AdapterHarness failedReplacement(server, &coordinator);
        auto failedPort = std::make_shared<IntegrationDisplayPort>(
            &failedReplacement.adapter, false);
        QVERIFY(coordinator.attachSession(displayIdentity(31), failedPort));
        bool rollbackPost = false;
        server.enqueue(displayPost(QStringLiteral("DP-1"), &rollbackPost,
                                   500));
        coordinator.sessionConnectionFailed(
            31, QStringLiteral("connection_failed"));
        QVERIFY(waitForAction(failedReplacement.adapter, complete));
        QVERIFY(rollbackPost);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, QStringLiteral("rollback_failed"));
        QCOMPARE(coordinator.phase(), DisplayPhase::Failed);
        QVERIFY(coordinator.recoveryVisible());
        QCOMPARE(displayPostCount(server), 2);
        QVERIFY(failedPort->disconnectEpochs.isEmpty());
        QTest::qWait(20);
        failedReplacement.adapter.pumpCompletions(
            PolarisAdapter::CompletionPumpLimit);
        QCOMPARE(displayPostCount(server), 2);
        QCOMPARE(server.pendingScriptCount(), 0);
        QCOMPARE(server.unexpectedRequestCount(), 0);
    }
}

void PolarisIntegrationTest::clipboardLimitsUtf8AcknowledgementAndLogsStaySafe()
{
    ScopedLogCapture logCapture;
    struct Observation
    {
        qint64 size = -1;
        QString errorCode;
        bool authenticated = false;
    };

    FakePolarisServer transportServer;
    QVERIFY(transportServer.start());
    QList<Observation> observations;
    {
        PolarisApiClient client(
            pairedComputer(transportServer), PolarisApiClient::TestIdentityTag{},
            transportServer.clientSslConfiguration());
        const auto fetch = [&](QByteArray body, qint64 limit) {
            FakePolarisServer::ResponseScript script;
            script.path = QStringLiteral("/actions/clipboard?type=text");
            script.headers.insert(QByteArrayLiteral("Content-Type"),
                                  QByteArrayLiteral("text/plain; charset=utf-8"));
            script.bodyChunks = {std::move(body)};
            transportServer.enqueue(std::move(script));
            client.fetchClipboard(limit,
                [&](auto, const PolarisResponse& response) {
                    observations.append({response.body.size(),
                                         response.errorCode,
                                         response.authenticated});
                });
            QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 1, 3000);
            QCOMPARE(client.drainCompletions(1), 1);
        };

        fetch({}, 16);
        fetch(QByteArrayLiteral("valid utf-8: \xE2\x9C\x93"), 32);
        fetch(QByteArray(16, 'a'), 16);
        fetch(QByteArray(17, 'b'), 16);
        fetch(QByteArray(1024 * 1024, 'c'), 1024 * 1024);
        fetch(QByteArray(1024 * 1024 + 1, 'd'), 1024 * 1024 + 1);
        QCOMPARE(client.drainCompletions(), 0);
    }
    QCOMPARE(observations.size(), 6);
    QCOMPARE(observations.at(0).size, 0);
    QCOMPARE(observations.at(1).errorCode, QString());
    QCOMPARE(observations.at(2).size, 16);
    QCOMPARE(observations.at(2).errorCode, QString());
    QCOMPARE(observations.at(3).errorCode,
             QStringLiteral("response_too_large"));
    QCOMPARE(observations.at(4).size, qint64(1024 * 1024));
    QCOMPARE(observations.at(4).errorCode, QString());
    QCOMPARE(observations.at(5).errorCode,
             QStringLiteral("response_too_large"));
    for (const Observation& observation : std::as_const(observations)) {
        QVERIFY(observation.authenticated);
    }

    FakePolarisServer actionServer;
    QVERIFY(actionServer.start());
    enqueueCurrentDiscovery(actionServer);
    AdapterHarness harness(actionServer);
    QVERIFY(harness.adapter.startDiscovery());
    QVERIFY(waitForDiscovery(harness.adapter));

    FakePolarisServer::ResponseScript invalidUtf8;
    invalidUtf8.path = QStringLiteral("/actions/clipboard?type=text");
    invalidUtf8.bodyChunks = {QByteArray::fromHex("c328")};
    actionServer.enqueue(std::move(invalidUtf8));
    ActionResult invalidResult;
    bool invalidComplete = false;
    harness.registry.execute(
        QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult& result) {
            invalidResult = result;
            invalidComplete = true;
        });
    QVERIFY(waitForAction(harness.adapter, invalidComplete));
    QCOMPARE(invalidResult.errorCode, QStringLiteral("invalid_utf8"));
    QVERIFY(harness.clipboardView->remoteText.isEmpty());

    for (const QByteArray& truncated : {
             QByteArray::fromHex("e2"), QByteArray::fromHex("e282")}) {
        FakePolarisServer::ResponseScript truncatedUtf8;
        truncatedUtf8.path = QStringLiteral("/actions/clipboard?type=text");
        truncatedUtf8.bodyChunks = {truncated};
        actionServer.enqueue(std::move(truncatedUtf8));
        ActionResult truncatedResult;
        bool truncatedComplete = false;
        harness.registry.execute(
            QStringLiteral("clipboard.fetch-remote"), {},
            [&](const ActionResult& result) {
                truncatedResult = result;
                truncatedComplete = true;
            });
        QVERIFY(waitForAction(harness.adapter, truncatedComplete));
        QCOMPARE(truncatedResult.errorCode, QStringLiteral("invalid_utf8"));
        QVERIFY(harness.clipboardView->remoteText.isEmpty());
    }

    FakePolarisServer::ResponseScript validClipboard;
    validClipboard.path = QStringLiteral("/actions/clipboard?type=text");
    validClipboard.bodyChunks = {QByteArrayLiteral("remote text")};
    actionServer.enqueue(std::move(validClipboard));
    ActionResult validResult;
    bool validComplete = false;
    harness.registry.execute(
        QStringLiteral("clipboard.fetch-remote"), {},
        [&](const ActionResult& result) {
            validResult = result;
            validComplete = true;
        });
    QVERIFY(waitForAction(harness.adapter, validComplete));
    QVERIFY(validResult.ok);
    QCOMPARE(harness.clipboardView->remoteText,
             QByteArrayLiteral("remote text"));

    const QByteArray clipboardSentinel(
        "PERIGEE_CLIPBOARD_SENTINEL_DO_NOT_LOG_7F13");
    harness.clipboardView->localText = clipboardSentinel;
    bool inspectedSentinel = false;
    FakePolarisServer::ResponseScript post;
    post.method = QByteArrayLiteral("POST");
    post.path = QStringLiteral("/actions/clipboard?type=text");
    post.status = 204;
    post.inspectBody = [&](QByteArray& body) {
        inspectedSentinel = body == clipboardSentinel;
        return inspectedSentinel;
    };
    actionServer.enqueue(std::move(post));
    ActionResult postResult;
    bool postComplete = false;
    harness.registry.execute(
        QStringLiteral("clipboard.send-local"), {},
        [&](const ActionResult& result) {
            postResult = result;
            postComplete = true;
        });
    QVERIFY(waitForAction(harness.adapter, postComplete));
    QVERIFY(inspectedSentinel);
    QVERIFY(postResult.ok);
    QVERIFY(actionServer.requestBodyWipeCount() > 0);
    QVERIFY(actionServer.allRequestBodyWipesWereZero());
    QVERIFY(harness.clipboardView->wipeCount > 0);
    QVERIFY(harness.clipboardView->allWipedBytesZero);

    FakePolarisServer::ResponseScript rejectedPost;
    rejectedPost.method = QByteArrayLiteral("POST");
    rejectedPost.path = QStringLiteral("/actions/clipboard?type=text");
    rejectedPost.status = 500;
    rejectedPost.inspectBody = [](QByteArray&) { return true; };
    actionServer.enqueue(std::move(rejectedPost));
    ActionResult rejectedResult;
    bool rejectedComplete = false;
    harness.registry.execute(
        QStringLiteral("clipboard.send-local"), {},
        [&](const ActionResult& result) {
            rejectedResult = result;
            rejectedComplete = true;
        });
    QVERIFY(waitForAction(harness.adapter, rejectedComplete));
    QCOMPARE(rejectedResult.errorCode,
             QStringLiteral("host_operation_failed"));

    const QString logs = logCapture.joined();
    QVERIFY2(!logs.contains(QString::fromLatin1(clipboardSentinel)),
             "clipboard sentinel escaped into the test log");
    QVERIFY2(!logs.contains(QStringLiteral("fixture-owner-token")),
             "session token escaped into the test log");
    QVERIFY2(!logs.contains(QStringLiteral("BEGIN PRIVATE KEY")),
             "private-key framing escaped into the test log");
    QVERIFY2(!logs.contains(QStringLiteral("Perigee-Test-Only-Client")),
             "client certificate marker escaped into the test log");
    for (const QString& fixtureName : {
             QStringLiteral("client.crt"),
             QStringLiteral("client.key")}) {
        const QByteArray pem = tlsFixture(fixtureName);
        const QByteArray base64 = pemPayload(pem);
        QVERIFY2(!logs.contains(QString::fromLatin1(pem)),
                 "client TLS PEM escaped into the test log");
        QVERIFY2(!base64.isEmpty() &&
                     !logs.contains(QString::fromLatin1(base64)),
                 "client TLS base64 escaped into the test log");
    }
    QCOMPARE(actionServer.unexpectedRequestCount(), 0);
    for (const FakePolarisServer::RequestRecord& record :
         actionServer.requestHistory()) {
        QVERIFY(!record.method.isEmpty());
        QVERIFY(!record.path.isEmpty());
    }
}

REGISTER_PERIGEE_TEST(PolarisIntegrationTest);

#include "test_polarisintegration.moc"
