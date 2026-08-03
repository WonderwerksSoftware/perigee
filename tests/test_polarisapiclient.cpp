#include "backend/identitymanager.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"
#include "perigee/polaris/polarisapiclient.h"
#include "test_registry.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
struct ReplyScript
{
    ReplyScript() = default;
    ReplyScript(int status, QList<QByteArray> bodyChunks,
                QSslCertificate peer)
        : httpStatus(status)
        , chunks(std::move(bodyChunks))
        , peerCertificate(std::move(peer))
    {
    }

    int httpStatus = 200;
    QList<QByteArray> chunks = {QByteArrayLiteral("{\"ok\":true}")};
    QSslCertificate peerCertificate;
    QList<QSslError> sslErrors;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    int delayMs = 0;
    bool emitEncrypted = true;
    bool neverFinish = false;
    bool controlled = false;
};

struct FakeNetworkState
{
    QMutex mutex;
    QQueue<ReplyScript> scripts;
    QList<QNetworkRequest> requests;
    QList<QByteArray> methods;
    QList<QByteArray> requestBodies;
    QList<QThread*> requestThreads;
    std::atomic_int liveManagers = 0;
    std::atomic_int liveReplies = 0;
    std::atomic_int abortCount = 0;
    std::atomic_llong bytesExposedBeforeAbort = -1;
    std::atomic_llong bytesReadByClient = 0;
    QSemaphore replyCreated;
    QList<QNetworkReply*> controlledReplies;
    QNetworkAccessManager* manager = nullptr;
};

QMutex capturedMessagesMutex;
QStringList capturedMessages;

void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message)
{
    QMutexLocker locker(&capturedMessagesMutex);
    capturedMessages.append(message);
}

class FakeReply final : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest& request, ReplyScript script,
              std::shared_ptr<FakeNetworkState> state, QObject* parent)
        : QNetworkReply(parent)
        , m_Script(std::move(script))
        , m_State(std::move(state))
    {
        m_State->liveReplies.fetch_add(1);
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute,
                     m_Script.httpStatus);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (m_Script.controlled) {
            QMutexLocker locker(&m_State->mutex);
            m_State->controlledReplies.append(this);
            m_State->replyCreated.release();
        }
        else if (!m_Script.neverFinish) {
            QTimer::singleShot(m_Script.delayMs, this,
                               [this] { beginScript(); });
        }
    }

    ~FakeReply() override
    {
        QMutexLocker locker(&m_State->mutex);
        m_State->controlledReplies.removeAll(this);
        m_State->liveReplies.fetch_sub(1);
    }

    QSslCertificate peerCertificate() const { return m_Script.peerCertificate; }

    void finishControlled() { beginScript(); }

    void abort() override
    {
        if (isFinished()) {
            return;
        }
        m_State->abortCount.fetch_add(1);
        m_State->bytesExposedBeforeAbort.store(m_BytesExposed);
        setError(QNetworkReply::OperationCanceledError,
                 QStringLiteral("aborted"));
        setFinished(true);
        emit errorOccurred(QNetworkReply::OperationCanceledError);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return m_Buffer.size() + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        const qint64 count = qMin(maximum,
                                  static_cast<qint64>(m_Buffer.size()));
        if (count == 0) {
            return isFinished() ? -1 : 0;
        }
        memcpy(data, m_Buffer.constData(), static_cast<size_t>(count));
        m_Buffer.remove(0, count);
        m_State->bytesReadByClient.fetch_add(count);
        return count;
    }

    void ignoreSslErrorsImplementation(
        const QList<QSslError>&) override
    {
        m_IgnoredSslErrors = true;
    }

private:
    void beginScript()
    {
        if (isFinished()) {
            return;
        }
        if (!m_Script.sslErrors.isEmpty()) {
            emit sslErrors(m_Script.sslErrors);
            if (!m_IgnoredSslErrors) {
                return;
            }
        }
        if (m_Script.emitEncrypted) {
            emit encrypted();
        }
        exposeNextChunk();
    }

    void exposeNextChunk()
    {
        if (isFinished()) {
            return;
        }
        if (!m_Script.chunks.isEmpty()) {
            const QByteArray chunk = m_Script.chunks.takeFirst();
            m_Buffer.append(chunk);
            m_BytesExposed += chunk.size();
            emit readyRead();
            if (isFinished()) {
                return;
            }
            QTimer::singleShot(0, this, [this] { exposeNextChunk(); });
            return;
        }
        if (m_Script.networkError != QNetworkReply::NoError) {
            setError(m_Script.networkError, QStringLiteral("fake network error"));
            emit errorOccurred(m_Script.networkError);
        }
        setFinished(true);
        emit finished();
    }

    ReplyScript m_Script;
    std::shared_ptr<FakeNetworkState> m_State;
    QByteArray m_Buffer;
    qint64 m_BytesExposed = 0;
    bool m_IgnoredSslErrors = false;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit FakeNetworkAccessManager(std::shared_ptr<FakeNetworkState> state,
                                      QObject* parent)
        : QNetworkAccessManager(parent), m_State(std::move(state))
    {
        m_State->liveManagers.fetch_add(1);
        QMutexLocker locker(&m_State->mutex);
        m_State->manager = this;
    }

    ~FakeNetworkAccessManager() override
    {
        QMutexLocker locker(&m_State->mutex);
        m_State->manager = nullptr;
        m_State->liveManagers.fetch_sub(1);
    }

protected:
    QNetworkReply* createRequest(Operation operation,
                                 const QNetworkRequest& request,
                                 QIODevice* outgoingData) override
    {
        ReplyScript script;
        {
            QMutexLocker locker(&m_State->mutex);
            m_State->requests.append(request);
            m_State->methods.append(
                operation == GetOperation
                    ? QByteArrayLiteral("GET")
                    : request.attribute(
                          QNetworkRequest::CustomVerbAttribute).toByteArray());
            m_State->requestBodies.append(
                outgoingData == nullptr
                    ? QByteArray()
                    : outgoingData->peek(1024 * 1024 + 1));
            m_State->requestThreads.append(QThread::currentThread());
            if (!m_State->scripts.isEmpty()) {
                script = m_State->scripts.dequeue();
            }
        }
        return new FakeReply(request, std::move(script), m_State, this);
    }

private:
    std::shared_ptr<FakeNetworkState> m_State;
};

class FakeNetworkBackend final : public PolarisNetworkBackend
{
public:
    explicit FakeNetworkBackend(std::shared_ptr<FakeNetworkState> state)
        : m_State(std::move(state)) {}

    QNetworkAccessManager* createNetworkAccessManager(
        QObject* parent) override
    {
        return new FakeNetworkAccessManager(m_State, parent);
    }

    QSslCertificate peerLeafCertificate(
        const QNetworkReply* reply) const override
    {
        const auto fake = dynamic_cast<const FakeReply*>(reply);
        return fake == nullptr ? QSslCertificate() : fake->peerCertificate();
    }

private:
    std::shared_ptr<FakeNetworkState> m_State;
};

QSslCertificate otherCertificate(const QSslCertificate& expected)
{
    const QList<QSslCertificate> certificates =
        QSslConfiguration::systemCaCertificates();
    for (const QSslCertificate& certificate : certificates) {
        if (!certificate.isNull() && certificate != expected) {
            return certificate;
        }
    }
    return QSslCertificate();
}

NvComputer pairedComputer(const QSslCertificate& certificate)
{
    NvComputer computer;
    computer.activeAddress = NvAddress(QStringLiteral("fd00::1234"), 47989);
    computer.activeHttpsPort = 47984;
    computer.serverCert = certificate;
    return computer;
}

void enqueue(const std::shared_ptr<FakeNetworkState>& state,
             ReplyScript script)
{
    QMutexLocker locker(&state->mutex);
    state->scripts.enqueue(std::move(script));
}

bool waitForCompletion(PolarisApiClient& client, int timeoutMs = 1000)
{
    QElapsedTimer timer;
    timer.start();
    while (client.pendingCompletionCount() == 0 &&
           timer.elapsed() < timeoutMs) {
        QThread::msleep(1);
    }
    return client.pendingCompletionCount() != 0;
}
}

class PolarisApiClientTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsExactPairedOrigins();
    void acceptsCanonicalAdvertisedEndpoints();
    void rejectsEndpointOriginAndPathEscapes_data();
    void rejectsEndpointOriginAndPathEscapes();
    void usesIdentitySslAndDedicatedNetworkThread();
    void acceptsOnlyExactCertificateSslErrors();
    void failsClosedForMissingMismatchedAndMixedIdentity();
    void classifiesPreTlsNetworkErrors_data();
    void classifiesPreTlsNetworkErrors();
    void classifiesAuthenticatedHttpReplyErrorsByStatus_data();
    void classifiesAuthenticatedHttpReplyErrorsByStatus();
    void retainsStatusBodyAndClassifiesJsonAndNetworkFailures();
    void classifiesEmptyExpectedJsonAsMalformed();
    void distinguishesTimeoutCancellationAndPolicyFailure();
    void abortsAtFirstByteOverClipboardLimit();
    void acceptsClipboardAtExactAdvertisedAndAbsoluteLimits();
    void rejectsClipboardWhenPairedOriginIsUnavailable();
    void enforcesAbsoluteClipboardLimitAndRetainsStatus();
    void boundsGenericResponseWithoutReadingPastFirstExcessByte();
    void postsJsonBodiesWithPairedIdentity();
    void rejectsUnsafeHttpMethodsBeforeNetworkUse();
    void rejectsGetBodiesBeforeNetworkUse();
    void deliversExactlyOnceOnlyWhenPolled();
    void preservesStableIdsAndPolledCompletionFifo();
    void rejectsCancellationAfterLocalTerminalQueueing();
    void rejectsCancellationAfterWorkerTerminalQueueing();
    void cancellationWinsBeforeControlledReplyCompletion();
    void selectedCallbacksCannotBeCancelledOrOvertaken();
    void recursiveDrainDoesNotOvertakeSelectedCallbacks();
    void callbackExceptionDoesNotLoseSelectedFifoCompletions();
    void callbackDeletionDiscardsRemainingSelectedCallbacks();
    void snapshotsComputerTupleUnderItsReadLock();
    void logsOnlyRedactedRequestMetadata();
    void logsChainedSensitivePathLabelsWithoutCanaries();
    void teardownStopsWorkerAndDiscardsCallbacks();
    void teardownDiscardsQueuedCompletion();
};

void PolarisApiClientTest::buildsExactPairedOrigins()
{
    QCOMPARE(PolarisApiClient::pairedOrigin(
                 NvAddress(QStringLiteral("10.20.30.40"), 47989), 47984),
             QUrl(QStringLiteral("https://10.20.30.40:47984")));
    QCOMPARE(PolarisApiClient::pairedOrigin(
                 NvAddress(QStringLiteral("fd00::1234"), 47989), 47984),
             QUrl(QStringLiteral("https://[fd00::1234]:47984")));
    QCOMPARE(PolarisApiClient::pairedOrigin(
                 NvAddress(QStringLiteral("polaris.lan"), 47989), 443),
             QUrl(QStringLiteral("https://polaris.lan:443")));
}

void PolarisApiClientTest::acceptsCanonicalAdvertisedEndpoints()
{
    const QUrl origin(QStringLiteral("https://[fd00::1234]:47984"));
    QString errorCode;

    QCOMPARE(PolarisApiClient::resolveAdvertisedEndpoint(
                 origin,
                 QStringLiteral("/polaris/v1/session/status?detail=full"),
                 &errorCode),
             QUrl(QStringLiteral(
                 "https://[fd00::1234]:47984/polaris/v1/session/status?detail=full")));
    QVERIFY(errorCode.isEmpty());

    QCOMPARE(PolarisApiClient::resolveAdvertisedEndpoint(
                 origin,
                 QStringLiteral("/polaris/v1/session/status?label=Desk%20One&token=a%2Fb"),
                 &errorCode),
             QUrl(QStringLiteral(
                 "https://[fd00::1234]:47984/polaris/v1/session/status?label=Desk%20One&token=a%2Fb")));
    QVERIFY(errorCode.isEmpty());

    QCOMPARE(PolarisApiClient::resolveAdvertisedEndpoint(
                 origin, QStringLiteral("/polaris/v1/"), &errorCode),
             QUrl(QStringLiteral(
                 "https://[fd00::1234]:47984/polaris/v1/")));
    QVERIFY(errorCode.isEmpty());
}

void PolarisApiClientTest::rejectsEndpointOriginAndPathEscapes_data()
{
    QTest::addColumn<QString>("endpoint");

    const QStringList rejected = {
        QStringLiteral("https://polaris.lan:47984/polaris/v1/status"),
        QStringLiteral("https://evil.invalid/polaris/v1/status"),
        QStringLiteral("//evil.invalid/polaris/v1/status"),
        QStringLiteral("//user@evil.invalid/polaris/v1/status"),
        QStringLiteral("user@evil.invalid/polaris/v1/status"),
        QStringLiteral("/polaris/v1/status#fragment"),
        QStringLiteral("/polaris/v1/../admin"),
        QStringLiteral("/polaris/v1/%2e%2e/admin"),
        QStringLiteral("/polaris/v1/a%2fb"),
        QStringLiteral("/polaris/v1/a%5cb"),
        QStringLiteral("/polaris/v1/a%3fb"),
        QStringLiteral("/polaris/v1/a%23b"),
        QStringLiteral("/polaris/v1/a%25b"),
        QStringLiteral("/polaris/v1/a\\b"),
        QStringLiteral("/polaris/v1/%ZZ"),
        QStringLiteral("/polaris/v1/status?value=%ZZ"),
        QStringLiteral("/polaris/v1//session/status"),
        QStringLiteral("/polaris/v1/session//status"),
        QStringLiteral("/polaris/v1"),
        QStringLiteral("/POLARIS/v1/status"),
        QStringLiteral("/actions/clipboard"),
    };
    for (const QString& endpoint : rejected) {
        QTest::newRow(endpoint.toUtf8().constData()) << endpoint;
    }
}

void PolarisApiClientTest::acceptsOnlyExactCertificateSslErrors()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript exact(200, {QByteArrayLiteral("{\"ok\":true}")}, expected);
    exact.sslErrors = {
        QSslError(QSslError::SelfSignedCertificate, expected),
        QSslError(QSslError::HostNameMismatch, expected),
    };
    enqueue(state, exact);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.get(QStringLiteral("/polaris/v1/status"), true,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QVERIFY(response.authenticated);
    QVERIFY(response.errorCode.isEmpty());
}

void PolarisApiClientTest::rejectsEndpointOriginAndPathEscapes()
{
    QFETCH(QString, endpoint);

    QString errorCode;
    const QUrl resolved = PolarisApiClient::resolveAdvertisedEndpoint(
        QUrl(QStringLiteral("https://polaris.lan:47984")), endpoint,
        &errorCode);

    QVERIFY2(resolved.isEmpty(), qPrintable(resolved.toString()));
    QCOMPARE(errorCode, QStringLiteral("endpoint_policy_rejected"));
}

void PolarisApiClientTest::usesIdentitySslAndDedicatedNetworkThread()
{
    const QSslConfiguration identity = IdentityManager::get()->getSslConfig();
    const QSslCertificate expected = identity.localCertificate();
    QVERIFY(!expected.isNull());
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{200, {QByteArrayLiteral("{\"value\":1}")},
                               expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));

    PolarisResponse response;
    QThread* callbackThread = nullptr;
    PolarisApiClient::RequestId requestId = 0;
    requestId = client.get(
        QStringLiteral("/polaris/v1/status"), true,
        [&](PolarisApiClient::RequestId id, const PolarisResponse& value) {
            QCOMPARE(id, requestId);
            response = value;
            callbackThread = QThread::currentThread();
        });

    QVERIFY(waitForCompletion(client));
    QVERIFY(callbackThread == nullptr);
    QCOMPARE(client.drainCompletions(1), 1);
    QCOMPARE(callbackThread, QThread::currentThread());
    QVERIFY(response.authenticated);
    QVERIFY(response.errorCode.isEmpty());
    QCOMPARE(response.json.object().value(QStringLiteral("value")).toInt(), 1);

    QMutexLocker locker(&state->mutex);
    QCOMPARE(state->requests.size(), 1);
    QCOMPARE(state->requestThreads.size(), 1);
    QVERIFY(state->requestThreads.first() != QThread::currentThread());
    const QNetworkRequest request = state->requests.first();
    QCOMPARE(request.url(), QUrl(QStringLiteral(
        "https://[fd00::1234]:47984/polaris/v1/status")));
    QCOMPARE(request.attribute(QNetworkRequest::Http2AllowedAttribute).toBool(),
             false);
    QCOMPARE(request.sslConfiguration().localCertificate(), expected);
    QCOMPARE(request.sslConfiguration().privateKey(), identity.privateKey());
}

void PolarisApiClientTest::failsClosedForMissingMismatchedAndMixedIdentity()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    const QSslCertificate other = otherCertificate(expected);
    QVERIFY(!other.isNull());
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript(200, {}, QSslCertificate()));
    enqueue(state, ReplyScript{200, {}, other});
    ReplyScript mixed{200, {}, expected};
    mixed.sslErrors = {QSslError(QSslError::SelfSignedCertificate, expected),
                       QSslError(QSslError::CertificateExpired, other)};
    enqueue(state, mixed);
    ReplyScript missingEncrypted{200, {}, expected};
    missingEncrypted.emitEncrypted = false;
    enqueue(state, missingEncrypted);
    ReplyScript handshakeFailure{0, {}, expected};
    handshakeFailure.emitEncrypted = false;
    handshakeFailure.networkError = QNetworkReply::SslHandshakeFailedError;
    enqueue(state, handshakeFailure);

    PolarisApiClient nullExpected(pairedComputer(QSslCertificate()),
        std::make_unique<FakeNetworkBackend>(state));
    QList<PolarisResponse> responses;
    nullExpected.get(QStringLiteral("/polaris/v1/status"), false,
        [&](auto, const PolarisResponse& response) { responses << response; });
    QVERIFY(waitForCompletion(nullExpected));
    nullExpected.drainCompletions();

    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    for (int i = 0; i < 5; ++i) {
        client.get(QStringLiteral("/polaris/v1/status"), false,
            [&](auto, const PolarisResponse& response) { responses << response; });
    }
    QTRY_VERIFY_WITH_TIMEOUT(client.pendingCompletionCount() == 5, 1000);
    client.drainCompletions();

    QCOMPARE(responses.size(), 6);
    for (const PolarisResponse& response : responses) {
        QCOMPARE(response.errorCode, QStringLiteral("tls_identity_mismatch"));
        QVERIFY(!response.authenticated);
    }
}

void PolarisApiClientTest::classifiesPreTlsNetworkErrors_data()
{
    QTest::addColumn<int>("networkError");
    QTest::newRow("host-not-found")
        << static_cast<int>(QNetworkReply::HostNotFoundError);
    QTest::newRow("connection-refused")
        << static_cast<int>(QNetworkReply::ConnectionRefusedError);
}

void PolarisApiClientTest::classifiesPreTlsNetworkErrors()
{
    QFETCH(int, networkError);
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript script{0, {}, expected};
    script.emitEncrypted = false;
    script.networkError =
        static_cast<QNetworkReply::NetworkError>(networkError);
    enqueue(state, script);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.get(QStringLiteral("/polaris/v1/status"), false,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QCOMPARE(response.errorCode, QStringLiteral("network_error"));
    QVERIFY(!response.authenticated);
}

void PolarisApiClientTest::classifiesAuthenticatedHttpReplyErrorsByStatus_data()
{
    QTest::addColumn<int>("httpStatus");
    QTest::addColumn<int>("networkError");
    QTest::addColumn<QByteArray>("body");

    QTest::newRow("not-found")
        << 404
        << static_cast<int>(QNetworkReply::ContentNotFoundError)
        << QByteArrayLiteral("{\"error\":\"missing\"}");
    QTest::newRow("service-unavailable")
        << 503
        << static_cast<int>(QNetworkReply::InternalServerError)
        << QByteArrayLiteral("{\"error\":\"busy\"}");
}

void PolarisApiClientTest::classifiesAuthenticatedHttpReplyErrorsByStatus()
{
    QFETCH(int, httpStatus);
    QFETCH(int, networkError);
    QFETCH(QByteArray, body);
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript script{httpStatus, {body}, expected};
    script.networkError =
        static_cast<QNetworkReply::NetworkError>(networkError);
    enqueue(state, script);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.get(QStringLiteral("/polaris/v1/capabilities"), false,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QVERIFY(response.authenticated);
    QCOMPARE(response.httpStatus, httpStatus);
    QCOMPARE(response.body, body);
    QCOMPARE(response.errorCode, QStringLiteral("http_error"));
}

void PolarisApiClientTest::retainsStatusBodyAndClassifiesJsonAndNetworkFailures()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{503, {QByteArrayLiteral("{\"error\":\"busy\"}")},
                               expected});
    enqueue(state, ReplyScript{200, {QByteArrayLiteral("not-json")}, expected});
    ReplyScript network{0, {}, expected};
    network.networkError = QNetworkReply::ConnectionRefusedError;
    enqueue(state, network);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));

    QHash<PolarisApiClient::RequestId, PolarisResponse> responses;
    const auto httpId = client.get(
        QStringLiteral("/polaris/v1/status"), true,
        [&](auto id, const PolarisResponse& response) {
            responses.insert(id, response);
        });
    const auto jsonId = client.get(
        QStringLiteral("/polaris/v1/status"), true,
        [&](auto id, const PolarisResponse& response) {
            responses.insert(id, response);
        });
    const auto networkId = client.get(
        QStringLiteral("/polaris/v1/status"), false,
        [&](auto id, const PolarisResponse& response) {
            responses.insert(id, response);
        });
    QTRY_VERIFY_WITH_TIMEOUT(client.pendingCompletionCount() == 3, 1000);
    client.drainCompletions();

    QCOMPARE(responses.value(httpId).httpStatus, 503);
    QCOMPARE(responses.value(httpId).body,
             QByteArrayLiteral("{\"error\":\"busy\"}"));
    QCOMPARE(responses.value(httpId).errorCode, QStringLiteral("http_error"));
    QCOMPARE(responses.value(jsonId).errorCode, QStringLiteral("malformed_json"));
    QCOMPARE(responses.value(jsonId).body, QByteArrayLiteral("not-json"));
    QCOMPARE(responses.value(networkId).errorCode, QStringLiteral("network_error"));
}

void PolarisApiClientTest::classifiesEmptyExpectedJsonAsMalformed()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{200, {}, expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.get(QStringLiteral("/polaris/v1/empty"), true,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QCOMPARE(response.httpStatus, 200);
    QVERIFY(response.body.isEmpty());
    QCOMPARE(response.errorCode, QStringLiteral("malformed_json"));
}

void PolarisApiClientTest::distinguishesTimeoutCancellationAndPolicyFailure()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript stalled;
    stalled.peerCertificate = expected;
    stalled.neverFinish = true;
    enqueue(state, stalled);
    enqueue(state, stalled);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisRequestOptions shortTimeout;
    shortTimeout.totalTimeoutMs = 20;
    shortTimeout.idleTimeoutMs = 20;
    QList<PolarisResponse> responses;

    client.get(QStringLiteral("/polaris/v1/timeout"), false,
        [&](auto, const PolarisResponse& response) { responses << response; },
        shortTimeout);
    const auto cancelled = client.get(
        QStringLiteral("/polaris/v1/cancel"), false,
        [&](auto, const PolarisResponse& response) { responses << response; });
    QVERIFY(client.cancel(cancelled));
    QVERIFY(!client.cancel(cancelled));
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/status"), false,
        [&](auto, const PolarisResponse& response) { responses << response; });

    QTRY_VERIFY_WITH_TIMEOUT(client.pendingCompletionCount() == 3, 1000);
    client.drainCompletions();
    QStringList errors;
    for (const PolarisResponse& response : responses) {
        errors << response.errorCode;
    }
    QVERIFY(errors.contains(QStringLiteral("timeout")));
    QVERIFY(errors.contains(QStringLiteral("cancelled")));
    QVERIFY(errors.contains(QStringLiteral("endpoint_policy_rejected")));
}

void PolarisApiClientTest::abortsAtFirstByteOverClipboardLimit()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{200,
        {QByteArrayLiteral("1234"), QByteArrayLiteral("5")}, expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.fetchClipboard(4,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QCOMPARE(response.errorCode, QStringLiteral("response_too_large"));
    QCOMPARE(response.httpStatus, 200);
    QCOMPARE(response.body, QByteArrayLiteral("1234"));
    QCOMPARE(state->abortCount.load(), 1);
    QCOMPARE(state->bytesExposedBeforeAbort.load(), 5);
    QMutexLocker locker(&state->mutex);
    QCOMPARE(state->requests.first().url().path(),
             QStringLiteral("/actions/clipboard"));
}

void PolarisApiClientTest::acceptsClipboardAtExactAdvertisedAndAbsoluteLimits()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{200, {QByteArrayLiteral("1234")}, expected});
    enqueue(state, ReplyScript{200, {QByteArray(1024 * 1024, 'a')}, expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<PolarisResponse> responses;

    client.fetchClipboard(4,
        [&](auto, const PolarisResponse& value) { responses.append(value); });
    client.fetchClipboard(std::nullopt,
        [&](auto, const PolarisResponse& value) { responses.append(value); });
    QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 2, 1000);
    client.drainCompletions();

    QCOMPARE(responses.size(), 2);
    QVERIFY(responses.at(0).errorCode.isEmpty());
    QCOMPARE(responses.at(0).body, QByteArrayLiteral("1234"));
    QVERIFY(responses.at(1).errorCode.isEmpty());
    QCOMPARE(responses.at(1).body.size(), 1024 * 1024);
    QCOMPARE(state->abortCount.load(), 0);
}

void PolarisApiClientTest::rejectsClipboardWhenPairedOriginIsUnavailable()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    NvComputer computer = pairedComputer(expected);
    computer.activeHttpsPort = 0;
    PolarisApiClient client(computer,
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.fetchClipboard(std::nullopt,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QCOMPARE(response.errorCode,
             QStringLiteral("endpoint_policy_rejected"));
    QMutexLocker locker(&state->mutex);
    QVERIFY(state->requests.isEmpty());
}

void PolarisApiClientTest::enforcesAbsoluteClipboardLimitAndRetainsStatus()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript(
        200, {QByteArray(1024 * 1024, 'a'), QByteArrayLiteral("b")}, expected));
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    client.fetchClipboard(2 * 1024 * 1024,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QCOMPARE(response.errorCode, QStringLiteral("response_too_large"));
    QCOMPARE(response.httpStatus, 200);
    QCOMPARE(response.body.size(), 1024 * 1024);
    QCOMPARE(state->bytesExposedBeforeAbort.load(), 1024 * 1024 + 1);
}

void PolarisApiClientTest::boundsGenericResponseWithoutReadingPastFirstExcessByte()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{
        200, {QByteArrayLiteral("1234"), QByteArray(64 * 1024, 'x')},
        expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisRequestOptions options;
    options.maxBodyBytes = 4;
    PolarisResponse response;

    client.get(QStringLiteral("/polaris/v1/bounded"), false,
        [&](auto, const PolarisResponse& value) { response = value; },
        options);
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QCOMPARE(response.errorCode, QStringLiteral("response_too_large"));
    QCOMPARE(response.httpStatus, 200);
    QCOMPARE(response.body, QByteArrayLiteral("1234"));
    QCOMPARE(state->bytesReadByClient.load(), 5);
}

void PolarisApiClientTest::postsJsonBodiesWithPairedIdentity()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{202, {QByteArrayLiteral("{\"accepted\":true}")},
                               expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    const QByteArray body = QByteArrayLiteral(
        "{\"session_token\":\"CANARY_REQUEST_BODY\"}");

    client.request(QByteArrayLiteral("post"),
        QStringLiteral("/polaris/v1/commands"), body, true,
        [](auto, const PolarisResponse&) {});
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();

    QMutexLocker locker(&state->mutex);
    QCOMPARE(state->methods, QList<QByteArray>{QByteArrayLiteral("POST")});
    QCOMPARE(state->requestBodies, QList<QByteArray>{body});
    QCOMPARE(state->requests.first().header(QNetworkRequest::ContentTypeHeader),
             QVariant(QStringLiteral("application/json")));
}

void PolarisApiClientTest::rejectsUnsafeHttpMethodsBeforeNetworkUse()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript(200, {}, expected));
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<PolarisResponse> responses;
    const QList<QByteArray> unsafeMethods = {
        QByteArrayLiteral("GET\nCANARY_METHOD_INJECTION"),
        QByteArrayLiteral(" GET"),
        QByteArrayLiteral("GET "),
        QByteArrayLiteral("G\tET"),
    };
    for (const QByteArray& method : unsafeMethods) {
        client.request(method, QStringLiteral("/polaris/v1/status"), {}, false,
            [&](auto, const PolarisResponse& value) {
                responses.append(value);
            });
    }
    QCOMPARE(client.pendingCompletionCount(), unsafeMethods.size());
    QCOMPARE(client.drainCompletions(), unsafeMethods.size());

    QCOMPARE(responses.size(), unsafeMethods.size());
    for (const PolarisResponse& response : responses) {
        QCOMPARE(response.errorCode,
                 QStringLiteral("endpoint_policy_rejected"));
    }
    QMutexLocker locker(&state->mutex);
    QVERIFY(state->requests.isEmpty());
}

void PolarisApiClientTest::rejectsGetBodiesBeforeNetworkUse()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript(200, {}, expected));
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;
    const QByteArray body = QByteArrayLiteral("CANARY_GET_BODY");
    {
        QMutexLocker locker(&capturedMessagesMutex);
        capturedMessages.clear();
    }
    const QtMessageHandler previous = qInstallMessageHandler(captureMessage);

    const auto id = client.request(QByteArrayLiteral("get"),
        QStringLiteral("/polaris/v1/status"), body, false,
        [&](auto, const PolarisResponse& value) { response = value; });
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();
    qInstallMessageHandler(previous);

    QCOMPARE(response.errorCode,
             QStringLiteral("endpoint_policy_rejected"));
    QVERIFY(!client.cancel(id));
    QMutexLocker stateLocker(&state->mutex);
    QVERIFY(state->requests.isEmpty());
    stateLocker.unlock();
    QMutexLocker messageLocker(&capturedMessagesMutex);
    QVERIFY(!capturedMessages.join(QLatin1Char('\n')).contains(
        QString::fromLatin1(body)));
}

void PolarisApiClientTest::deliversExactlyOnceOnlyWhenPolled()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript delayed{200, {QByteArrayLiteral("ok")}, expected};
    delayed.delayMs = 5;
    enqueue(state, delayed);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    int callbackCount = 0;
    const auto id = client.get(QStringLiteral("/polaris/v1/race"), false,
        [&](auto, const PolarisResponse&) {
            ++callbackCount;
            (void)client.pendingCompletionCount();
        });
    client.cancel(id);

    QVERIFY(waitForCompletion(client));
    QCOMPARE(callbackCount, 0);
    QCOMPARE(client.drainCompletions(1), 1);
    QCOMPARE(callbackCount, 1);
    QTest::qWait(20);
    QCOMPARE(client.drainCompletions(), 0);
    QCOMPARE(callbackCount, 1);
}

void PolarisApiClientTest::preservesStableIdsAndPolledCompletionFifo()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    for (int i = 0; i < 3; ++i) {
        enqueue(state, ReplyScript{200, {QByteArray::number(i)}, expected});
    }
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<PolarisApiClient::RequestId> callbackIds;
    QList<PolarisApiClient::RequestId> requestIds;

    for (int i = 0; i < 3; ++i) {
        requestIds.append(client.get(
            QStringLiteral("/polaris/v1/fifo?index=%1").arg(i), false,
            [&](auto id, const PolarisResponse&) { callbackIds.append(id); }));
    }
    QCOMPARE(requestIds.at(1), requestIds.at(0) + 1);
    QCOMPARE(requestIds.at(2), requestIds.at(1) + 1);
    QTRY_COMPARE_WITH_TIMEOUT(client.pendingCompletionCount(), 3, 1000);
    QCOMPARE(client.drainCompletions(2), 2);
    QCOMPARE(callbackIds, requestIds.mid(0, 2));
    QCOMPARE(client.pendingCompletionCount(), 1);
    QCOMPARE(client.drainCompletions(2), 1);
    QCOMPARE(callbackIds, requestIds);
}

void PolarisApiClientTest::rejectsCancellationAfterLocalTerminalQueueing()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    const auto localId = client.get(
        QStringLiteral("https://evil.invalid/polaris/v1/status"), false,
        [&](auto, const PolarisResponse& value) {
            response = value;
        });
    QVERIFY(!client.cancel(localId));
    QCOMPARE(client.drainCompletions(), 1);
    QCOMPARE(response.errorCode,
             QStringLiteral("endpoint_policy_rejected"));
}

void PolarisApiClientTest::rejectsCancellationAfterWorkerTerminalQueueing()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript controlled{200, {}, expected};
    controlled.controlled = true;
    enqueue(state, controlled);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisResponse response;

    const auto workerId = client.get(QStringLiteral("/polaris/v1/status"), false,
        [&](auto, const PolarisResponse& value) {
            response = value;
        });
    QVERIFY(state->replyCreated.tryAcquire(1, 1000));
    FakeReply* reply = nullptr;
    {
        QMutexLocker locker(&state->mutex);
        reply = static_cast<FakeReply*>(state->controlledReplies.constFirst());
    }
    QVERIFY(QMetaObject::invokeMethod(reply,
        [reply] { reply->finishControlled(); },
        Qt::BlockingQueuedConnection));
    QCOMPARE(client.pendingCompletionCount(), 1);
    QVERIFY(!client.cancel(workerId));
    QCOMPARE(client.drainCompletions(), 1);
    QVERIFY(response.errorCode.isEmpty());
}

void PolarisApiClientTest::cancellationWinsBeforeControlledReplyCompletion()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript controlled{200, {QByteArrayLiteral("late")}, expected};
    controlled.controlled = true;
    enqueue(state, controlled);
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<PolarisResponse> responses;

    const auto id = client.get(QStringLiteral("/polaris/v1/status"), false,
        [&](auto, const PolarisResponse& response) {
            responses.append(response);
        });
    QVERIFY(state->replyCreated.tryAcquire(1, 1000));
    QVERIFY(client.cancel(id));
    QVERIFY(!client.cancel(id));
    QNetworkAccessManager* manager = nullptr;
    {
        QMutexLocker locker(&state->mutex);
        manager = state->manager;
    }
    QVERIFY(QMetaObject::invokeMethod(manager, [] {},
                                     Qt::BlockingQueuedConnection));

    QCOMPARE(client.pendingCompletionCount(), 1);
    QCOMPARE(client.drainCompletions(), 1);
    QCOMPARE(responses.size(), 1);
    QCOMPARE(responses.first().errorCode, QStringLiteral("cancelled"));
    QCOMPARE(state->abortCount.load(), 1);
    QCOMPARE(client.drainCompletions(), 0);
}

void PolarisApiClientTest::selectedCallbacksCannotBeCancelledOrOvertaken()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    for (int i = 0; i < 2; ++i) {
        ReplyScript controlled{200, {}, expected};
        controlled.controlled = true;
        enqueue(state, controlled);
    }
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<QString> order;
    bool selectedCancelResult = true;
    int recursiveDrainResult = -1;
    PolarisApiClient::RequestId secondId = 0;
    client.get(QStringLiteral("/polaris/v1/first"), false,
        [&](auto, const PolarisResponse&) {
            order.append(QStringLiteral("A"));
            selectedCancelResult = client.cancel(secondId);
            recursiveDrainResult = client.drainCompletions();
        });
    secondId = client.get(QStringLiteral("/polaris/v1/second"), false,
        [&](auto, const PolarisResponse&) { order.append(QStringLiteral("B")); });

    QVERIFY(state->replyCreated.tryAcquire(2, 1000));
    QList<FakeReply*> replies;
    {
        QMutexLocker locker(&state->mutex);
        for (QNetworkReply* reply : state->controlledReplies) {
            replies.append(static_cast<FakeReply*>(reply));
        }
    }
    QCOMPARE(replies.size(), 2);
    for (FakeReply* reply : replies) {
        QVERIFY(QMetaObject::invokeMethod(reply,
            [reply] { reply->finishControlled(); },
            Qt::BlockingQueuedConnection));
    }
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/third"), false,
        [&](auto, const PolarisResponse&) { order.append(QStringLiteral("C")); });
    QCOMPARE(client.pendingCompletionCount(), 3);

    QCOMPARE(client.drainCompletions(2), 2);
    QVERIFY(!selectedCancelResult);
    QCOMPARE(recursiveDrainResult, 0);
    QCOMPARE(order, QList<QString>({QStringLiteral("A"),
                                   QStringLiteral("B")}));
    QCOMPARE(client.pendingCompletionCount(), 1);
    QCOMPARE(client.drainCompletions(), 1);
    QCOMPARE(order, QList<QString>({QStringLiteral("A"),
                                   QStringLiteral("B"),
                                   QStringLiteral("C")}));
}

void PolarisApiClientTest::recursiveDrainDoesNotOvertakeSelectedCallbacks()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<QString> order;
    int recursiveDrainResult = -1;
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/first"), false,
        [&](auto, const PolarisResponse&) {
            order.append(QStringLiteral("A"));
            recursiveDrainResult = client.drainCompletions();
        });
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/second"), false,
        [&](auto, const PolarisResponse&) { order.append(QStringLiteral("B")); });
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/third"), false,
        [&](auto, const PolarisResponse&) { order.append(QStringLiteral("C")); });

    QCOMPARE(client.drainCompletions(2), 2);
    QCOMPARE(recursiveDrainResult, 0);
    QCOMPARE(order, QList<QString>({QStringLiteral("A"),
                                   QStringLiteral("B")}));
    QCOMPARE(client.pendingCompletionCount(), 1);
    QCOMPARE(client.drainCompletions(), 1);
    QCOMPARE(order, QList<QString>({QStringLiteral("A"),
                                   QStringLiteral("B"),
                                   QStringLiteral("C")}));
}

void PolarisApiClientTest::callbackExceptionDoesNotLoseSelectedFifoCompletions()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    QList<QString> order;
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/first"), false,
        [&](auto, const PolarisResponse&) {
            order.append(QStringLiteral("A"));
            throw std::runtime_error(
                "CANARY_EXCEPTION_TEXT CANARY_REQUEST_BODY CANARY_RESPONSE_BODY");
        });
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/second"), false,
        [&](auto, const PolarisResponse&) { order.append(QStringLiteral("B")); });
    client.get(QStringLiteral("https://evil.invalid/polaris/v1/third"), false,
        [&](auto, const PolarisResponse&) { order.append(QStringLiteral("C")); });
    {
        QMutexLocker locker(&capturedMessagesMutex);
        capturedMessages.clear();
    }
    const QtMessageHandler previous = qInstallMessageHandler(captureMessage);

    bool exceptionEscaped = false;
    int drained = -1;
    try {
        drained = client.drainCompletions(2);
    }
    catch (...) {
        exceptionEscaped = true;
    }
    qInstallMessageHandler(previous);

    QVERIFY(!exceptionEscaped);
    QCOMPARE(drained, 2);
    QCOMPARE(order, QList<QString>({QStringLiteral("A"),
                                   QStringLiteral("B")}));
    QCOMPARE(client.pendingCompletionCount(), 1);
    QCOMPARE(client.drainCompletions(), 1);
    QCOMPARE(order, QList<QString>({QStringLiteral("A"),
                                   QStringLiteral("B"),
                                   QStringLiteral("C")}));
    QMutexLocker locker(&capturedMessagesMutex);
    const QString log = capturedMessages.join(QLatin1Char('\n'));
    QVERIFY(log.contains(QStringLiteral("callback failed")));
    QVERIFY(!log.contains(QStringLiteral("CANARY")));
}

void PolarisApiClientTest::callbackDeletionDiscardsRemainingSelectedCallbacks()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    int firstCallbacks = 0;
    int secondCallbacks = 0;
    auto* client = new PolarisApiClient(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    PolarisApiClient* ownedClient = client;
    client->get(QStringLiteral("https://evil.invalid/polaris/v1/first"), false,
        [&, ownedClient](auto, const PolarisResponse&) {
            ++firstCallbacks;
            delete ownedClient;
            client = nullptr;
        });
    client->get(QStringLiteral("https://evil.invalid/polaris/v1/second"), false,
        [&](auto, const PolarisResponse&) { ++secondCallbacks; });

    QCOMPARE(client->pendingCompletionCount(), 2);
    client->drainCompletions(2);

    QCOMPARE(client, nullptr);
    QCOMPARE(firstCallbacks, 1);
    QCOMPARE(secondCallbacks, 0);
    QCOMPARE(state->liveManagers.load(), 0);
}

void PolarisApiClientTest::snapshotsComputerTupleUnderItsReadLock()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    NvComputer computer = pairedComputer(expected);
    auto state = std::make_shared<FakeNetworkState>();
    QSemaphore ready;
    QSemaphore start;
    QSemaphore finished;
    std::thread constructorThread([&] {
        ready.release();
        start.acquire();
        {
            PolarisApiClient client(computer,
                std::make_unique<FakeNetworkBackend>(state));
        }
        finished.release();
    });

    const bool readyObserved = ready.tryAcquire(1, 1000);
    computer.lock.lockForWrite();
    start.release();
    const bool finishedWhileWriteLocked = finished.tryAcquire(1, 250);
    computer.lock.unlock();
    bool finishedAfterUnlock = true;
    if (!finishedWhileWriteLocked) {
        finishedAfterUnlock = finished.tryAcquire(1, 1000);
    }
    constructorThread.join();

    QVERIFY(readyObserved);
    QVERIFY(finishedAfterUnlock);
    QVERIFY(!finishedWhileWriteLocked);
}

void PolarisApiClientTest::logsOnlyRedactedRequestMetadata()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{
        503, {QByteArrayLiteral("CANARY_RESPONSE_BODY")}, expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    {
        QMutexLocker locker(&capturedMessagesMutex);
        capturedMessages.clear();
    }
    const QtMessageHandler previous = qInstallMessageHandler(captureMessage);

    const QByteArray requestBody = QByteArrayLiteral(
        "{\"session_token\":\"CANARY_REQUEST_BODY\","
        "\"private_key\":\"CANARY_PRIVATE_KEY\","
        "\"certificate\":\"CANARY_CLIENT_CERTIFICATE_BYTES\"}");
    client.request(QByteArrayLiteral("POST"),
        QStringLiteral("/polaris/v1/status?token=CANARY_QUERY_SECRET"),
        requestBody, false, [](auto, const PolarisResponse&) {});
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();
    qInstallMessageHandler(previous);

    QString log;
    {
        QMutexLocker locker(&capturedMessagesMutex);
        log = capturedMessages.join(QLatin1Char('\n'));
    }
    QVERIFY(log.contains(QStringLiteral("Polaris POST")));
    QVERIFY(log.contains(QStringLiteral("status=503")));
    QVERIFY(log.contains(QStringLiteral("error=http_error")));
    QVERIFY(!log.contains(QStringLiteral("CANARY_QUERY_SECRET")));
    QVERIFY(!log.contains(QStringLiteral("CANARY_RESPONSE_BODY")));
    QVERIFY(!log.contains(QStringLiteral("CANARY_REQUEST_BODY")));
    QVERIFY(!log.contains(QStringLiteral("CANARY_PRIVATE_KEY")));
    QVERIFY(!log.contains(QStringLiteral("CANARY_CLIENT_CERTIFICATE_BYTES")));
}

void PolarisApiClientTest::logsChainedSensitivePathLabelsWithoutCanaries()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{
        200, {QByteArrayLiteral("{\"ok\":true}")}, expected});
    PolarisApiClient client(pairedComputer(expected),
        std::make_unique<FakeNetworkBackend>(state));
    {
        QMutexLocker locker(&capturedMessagesMutex);
        capturedMessages.clear();
    }
    const QtMessageHandler previous = qInstallMessageHandler(captureMessage);

    client.get(
        QStringLiteral(
            "/polaris/v1/session-token/authorization-key/"
            "CANARY_CHAINED_LOG_SECRET"),
        true, [](auto, const PolarisResponse&) {});
    QVERIFY(waitForCompletion(client));
    client.drainCompletions();
    qInstallMessageHandler(previous);

    QString log;
    {
        QMutexLocker locker(&capturedMessagesMutex);
        log = capturedMessages.join(QLatin1Char('\n'));
    }
    QVERIFY(log.contains(QStringLiteral("Polaris GET")));
    QVERIFY(log.contains(QStringLiteral(
        "/polaris/v1/session-token/<redacted>/<redacted>")));
    QVERIFY(!log.contains(QStringLiteral("CANARY_CHAINED_LOG_SECRET")));
}

void PolarisApiClientTest::teardownStopsWorkerAndDiscardsCallbacks()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    ReplyScript stalled;
    stalled.peerCertificate = expected;
    stalled.neverFinish = true;
    enqueue(state, stalled);
    int callbackCount = 0;
    {
        PolarisApiClient client(pairedComputer(expected),
            std::make_unique<FakeNetworkBackend>(state));
        client.get(QStringLiteral("/polaris/v1/stalled"), false,
            [&](auto, const PolarisResponse&) { ++callbackCount; });
        QTRY_COMPARE_WITH_TIMEOUT(state->liveReplies.load(), 1, 1000);
    }

    QCOMPARE(callbackCount, 0);
    QCOMPARE(state->liveReplies.load(), 0);
    QCOMPARE(state->liveManagers.load(), 0);
    QVERIFY(state->abortCount.load() >= 1);
}

void PolarisApiClientTest::teardownDiscardsQueuedCompletion()
{
    const QSslCertificate expected =
        IdentityManager::get()->getSslConfig().localCertificate();
    auto state = std::make_shared<FakeNetworkState>();
    enqueue(state, ReplyScript{200, {QByteArrayLiteral("queued")}, expected});
    int callbackCount = 0;
    {
        PolarisApiClient client(pairedComputer(expected),
            std::make_unique<FakeNetworkBackend>(state));
        client.get(QStringLiteral("/polaris/v1/queued"), false,
            [&](auto, const PolarisResponse&) { ++callbackCount; });
        QVERIFY(waitForCompletion(client));
    }

    QCOMPARE(callbackCount, 0);
    QCOMPARE(state->liveReplies.load(), 0);
    QCOMPARE(state->liveManagers.load(), 0);
}

REGISTER_PERIGEE_TEST(PolarisApiClientTest);

#include "test_polarisapiclient.moc"
