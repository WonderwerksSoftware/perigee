#include "polarisapiclient.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "perigee/support/redaction.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonParseError>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QQueue>
#include <QReadLocker>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <vector>

namespace
{
constexpr qint64 kAbsoluteBodyLimit = 1024 * 1024;

using WipeObserver = std::function<void(const QByteArray&)>;

QByteArray wipeOwnedBytesDeferred(QByteArray& bytes)
{
    const qsizetype capacity = bytes.capacity();
    if (capacity > bytes.size()) {
        bytes.resize(capacity);
    }
    volatile unsigned char* data =
        reinterpret_cast<volatile unsigned char*>(bytes.data());
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        data[index] = 0;
    }
    QByteArray wiped(bytes.size(), '\0');
    bytes.clear();
    bytes.squeeze();
    return wiped;
}

void wipeOwnedBytes(QByteArray& bytes, const WipeObserver& observer)
{
    const QByteArray wiped = wipeOwnedBytesDeferred(bytes);
    if (observer) {
        try {
            observer(wiped);
        }
        catch (...) {
            // Diagnostic observers must not interrupt erasure paths.
        }
    }
}

class ScopedOwnedBytesWipe final
{
public:
    ScopedOwnedBytesWipe(
        QByteArray& bytes, bool sensitive, WipeObserver observer)
        : m_Bytes(bytes)
        , m_Sensitive(sensitive)
        , m_Observer(std::move(observer))
    {
    }

    ~ScopedOwnedBytesWipe()
    {
        if (m_Sensitive) {
            wipeOwnedBytes(m_Bytes, m_Observer);
        }
    }

    ScopedOwnedBytesWipe(const ScopedOwnedBytesWipe&) = delete;
    ScopedOwnedBytesWipe& operator=(const ScopedOwnedBytesWipe&) = delete;

private:
    QByteArray& m_Bytes;
    bool m_Sensitive = false;
    WipeObserver m_Observer;
};

struct CompletionEntry
{
    CompletionEntry() = default;
    CompletionEntry(PolarisApiClient::RequestId id,
                    PolarisResponse terminalResponse,
                    bool wipeBody, WipeObserver wipeObserver)
        : requestId(id)
        , response(std::move(terminalResponse))
        , sensitiveBody(wipeBody)
        , observer(std::move(wipeObserver))
    {
    }
    CompletionEntry(const CompletionEntry&) = delete;
    CompletionEntry& operator=(const CompletionEntry&) = delete;
    CompletionEntry(CompletionEntry&& other) noexcept
        : requestId(other.requestId)
        , response(std::move(other.response))
        , sensitiveBody(other.sensitiveBody)
        , observer(std::move(other.observer))
    {
        other.sensitiveBody = false;
    }
    CompletionEntry& operator=(CompletionEntry&& other) noexcept
    {
        if (this != &other) {
            wipeBody();
            requestId = other.requestId;
            response = std::move(other.response);
            sensitiveBody = other.sensitiveBody;
            observer = std::move(other.observer);
            other.sensitiveBody = false;
        }
        return *this;
    }
    ~CompletionEntry() { wipeBody(); }

    void wipeBody()
    {
        if (sensitiveBody) {
            wipeOwnedBytes(response.body, observer);
            sensitiveBody = false;
        }
    }

    PolarisApiClient::RequestId requestId = 0;
    PolarisResponse response;
    bool sensitiveBody = false;
    WipeObserver observer;
};

enum class RequestPhase
{
    Submitted,
    CancelRequested,
    TerminalQueued,
};

struct RequestRecord
{
    PolarisApiClient::Completion callback;
    RequestPhase phase = RequestPhase::Submitted;
};

struct SharedRequestState
{
    mutable QMutex mutex;
    std::deque<CompletionEntry> queue;
    QHash<PolarisApiClient::RequestId, RequestRecord> requests;
    bool accepting = true;
    bool draining = false;
};

enum class TerminalClaim
{
    Rejected,
    Original,
    Cancelled,
};

QString userMessageForError(const QString& errorCode);

TerminalClaim queueTerminal(
    const std::shared_ptr<SharedRequestState>& state,
    PolarisApiClient::RequestId requestId,
    PolarisResponse& response, bool sensitiveBody = false,
    WipeObserver observer = {})
{
    QMutexLocker locker(&state->mutex);
    auto iterator = state->requests.find(requestId);
    if (!state->accepting || iterator == state->requests.end() ||
            iterator->phase == RequestPhase::TerminalQueued) {
        QByteArray wiped;
        if (sensitiveBody) {
            wiped = wipeOwnedBytesDeferred(response.body);
        }
        locker.unlock();
        if (observer && sensitiveBody) {
            observer(wiped);
        }
        return TerminalClaim::Rejected;
    }

    TerminalClaim claim = TerminalClaim::Original;
    if (iterator->phase == RequestPhase::CancelRequested) {
        const bool hadSensitiveBody = sensitiveBody;
        QByteArray wiped;
        if (sensitiveBody) {
            wiped = wipeOwnedBytesDeferred(response.body);
            sensitiveBody = false;
        }
        PolarisResponse cancelled;
        cancelled.authenticated = response.authenticated;
        cancelled.errorCode = QStringLiteral("cancelled");
        cancelled.userMessage = userMessageForError(cancelled.errorCode);
        response = std::move(cancelled);
        claim = TerminalClaim::Cancelled;
        iterator->phase = RequestPhase::TerminalQueued;
        state->queue.emplace_back(requestId, std::move(response),
                                  false, WipeObserver());
        locker.unlock();
        if (observer && hadSensitiveBody) {
            observer(wiped);
        }
        return claim;
    }
    iterator->phase = RequestPhase::TerminalQueued;
    state->queue.emplace_back(requestId, std::move(response),
                              sensitiveBody, std::move(observer));
    return claim;
}

struct WorkerRequest
{
    WorkerRequest() = default;
    WorkerRequest(PolarisApiClient::RequestId id, QByteArray verb,
                  QUrl requestUrl, QString path, QByteArray requestBody,
                  QString requestContentType, bool json,
                  PolarisRequestOptions requestOptions,
                  WipeObserver wipeObserver,
                  bool wipeRequestBody, bool wipeResponseBody)
        : requestId(id)
        , method(std::move(verb))
        , url(std::move(requestUrl))
        , logPath(std::move(path))
        , body(std::move(requestBody))
        , contentType(std::move(requestContentType))
        , expectJson(json)
        , options(requestOptions)
        , observer(wipeObserver)
        , sensitiveRequestBody(wipeRequestBody)
        , sensitiveResponseBody(wipeResponseBody)
    {
    }

    WorkerRequest(const WorkerRequest&) = delete;
    WorkerRequest& operator=(const WorkerRequest&) = delete;

    WorkerRequest(WorkerRequest&& other) noexcept
    {
        *this = std::move(other);
    }

    WorkerRequest& operator=(WorkerRequest&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        wipeRequestBytes();
        requestId = other.requestId;
        method = std::move(other.method);
        url = std::move(other.url);
        logPath = std::move(other.logPath);
        body = std::move(other.body);
        contentType = std::move(other.contentType);
        expectJson = other.expectJson;
        options = other.options;
        observer = other.observer;
        sensitiveRequestBody = other.sensitiveRequestBody;
        sensitiveResponseBody = other.sensitiveResponseBody;
        other.observer = nullptr;
        other.sensitiveRequestBody = false;
        other.sensitiveResponseBody = false;
        return *this;
    }

    ~WorkerRequest()
    {
        wipeRequestBytes();
    }

    void wipeRequestBytes()
    {
        if (sensitiveRequestBody) {
            wipeOwnedBytes(body, observer);
            sensitiveRequestBody = false;
        }
    }

    PolarisApiClient::RequestId requestId = 0;
    QByteArray method;
    QUrl url;
    QString logPath;
    QByteArray body;
    QString contentType = QStringLiteral("application/json");
    bool expectJson = false;
    PolarisRequestOptions options;
    WipeObserver observer;
    bool sensitiveRequestBody = false;
    bool sensitiveResponseBody = false;
};

class QtPolarisNetworkBackend final : public PolarisNetworkBackend
{
public:
    QNetworkAccessManager* createNetworkAccessManager(
        QObject* parent) override
    {
        return new QNetworkAccessManager(parent);
    }

    QSslCertificate peerLeafCertificate(
        const QNetworkReply* reply) const override
    {
        return reply == nullptr
            ? QSslCertificate()
            : reply->sslConfiguration().peerCertificate();
    }
};

QString userMessageForError(const QString& errorCode)
{
    if (errorCode == QStringLiteral("endpoint_policy_rejected")) {
        return QStringLiteral("Polaris supplied an unsafe endpoint.");
    }
    if (errorCode == QStringLiteral("tls_identity_mismatch")) {
        return QStringLiteral("The Polaris host identity did not match the paired host.");
    }
    if (errorCode == QStringLiteral("timeout")) {
        return QStringLiteral("The Polaris request timed out.");
    }
    if (errorCode == QStringLiteral("cancelled")) {
        return QStringLiteral("The Polaris request was cancelled.");
    }
    if (errorCode == QStringLiteral("network_error")) {
        return QStringLiteral("Polaris is unreachable.");
    }
    if (errorCode == QStringLiteral("http_error")) {
        return QStringLiteral("Polaris rejected the request.");
    }
    if (errorCode == QStringLiteral("malformed_json")) {
        return QStringLiteral("Polaris returned malformed JSON.");
    }
    if (errorCode == QStringLiteral("response_too_large")) {
        return QStringLiteral("The Polaris response exceeded the safe size limit.");
    }
    return {};
}

QUrl rejectEndpoint(QString* errorCode)
{
    if (errorCode != nullptr) {
        *errorCode = QStringLiteral("endpoint_policy_rejected");
    }
    return {};
}

bool isHexDigit(QChar character)
{
    const QChar lower = character.toLower();
    return character.isDigit() ||
           (lower >= QLatin1Char('a') && lower <= QLatin1Char('f'));
}

bool hasMalformedPercentEscape(const QString& input)
{
    for (qsizetype i = 0; i < input.size(); ++i) {
        if (input.at(i) != QLatin1Char('%')) {
            continue;
        }
        if (i + 2 >= input.size() ||
                !isHexDigit(input.at(i + 1)) ||
                !isHexDigit(input.at(i + 2))) {
            return true;
        }
        i += 2;
    }
    return false;
}

bool hasControlCharacter(const QString& input)
{
    for (const QChar character : input) {
        if (character.category() == QChar::Other_Control ||
                character == QChar::LineSeparator ||
                character == QChar::ParagraphSeparator) {
            return true;
        }
    }
    return false;
}

class PolarisNetworkWorker final : public QObject
{
public:
    PolarisNetworkWorker(
        QSslCertificate expectedLeaf, QSslConfiguration identity,
        std::unique_ptr<PolarisNetworkBackend> backend,
        std::shared_ptr<SharedRequestState> state)
        : m_ExpectedLeaf(std::move(expectedLeaf))
        , m_Identity(std::move(identity))
        , m_Backend(std::move(backend))
        , m_State(std::move(state))
    {
    }

    void initialize()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        m_Manager = m_Backend->createNetworkAccessManager(this);
        m_Manager->setProxy(QNetworkProxy::NoProxy);
        m_Manager->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    }

    void startRequest(WorkerRequest request)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        request.observer = m_Backend->sensitiveBufferObserver();
        RequestPhase phase = RequestPhase::TerminalQueued;
        {
            QMutexLocker locker(&m_State->mutex);
            const auto iterator = m_State->requests.constFind(
                request.requestId);
            if (!m_State->accepting ||
                    iterator == m_State->requests.constEnd()) {
                return;
            }
            phase = iterator->phase;
        }
        if (m_ShuttingDown || phase == RequestPhase::TerminalQueued) {
            return;
        }
        if (phase == RequestPhase::CancelRequested) {
            PolarisResponse response;
            response.errorCode = QStringLiteral("cancelled");
            response.userMessage = userMessageForError(response.errorCode);
            if (queueTerminal(m_State, request.requestId, response) !=
                    TerminalClaim::Rejected) {
                logCompletion(request, 0, response.errorCode, 0);
            }
            return;
        }
        if (m_ExpectedLeaf.isNull()) {
            PolarisResponse response;
            response.errorCode = QStringLiteral("tls_identity_mismatch");
            response.userMessage = userMessageForError(response.errorCode);
            if (queueTerminal(m_State, request.requestId, response) !=
                    TerminalClaim::Rejected) {
                logCompletion(request, 0, response.errorCode, 0);
            }
            return;
        }

        auto active = std::make_unique<ActiveRequest>();
        active->request = std::move(request);
        active->elapsed.start();
        WorkerRequest& activeRequest = active->request;
        QNetworkRequest networkRequest =
            PolarisApiClient::makeRequest(activeRequest.url, m_Identity);
        if (activeRequest.method == QByteArrayLiteral("GET")) {
            active->reply = m_Manager->get(networkRequest);
        }
        else {
            networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                     activeRequest.contentType);
            active->reply = m_Manager->sendCustomRequest(
                networkRequest, activeRequest.method, activeRequest.body);
        }
        QNetworkReply* reply = active->reply;
        const auto requestId = activeRequest.requestId;

        active->totalTimer = new QTimer(this);
        active->totalTimer->setSingleShot(true);
        active->idleTimer = new QTimer(this);
        active->idleTimer->setSingleShot(true);
        connect(active->totalTimer, &QTimer::timeout, this,
                [this, requestId] { timeout(requestId); });
        connect(active->idleTimer, &QTimer::timeout, this,
                [this, requestId] { timeout(requestId); });
        connect(reply, &QNetworkReply::sslErrors, this,
                [this, requestId](const QList<QSslError>& errors) {
            handleSslErrors(requestId, errors);
        });
        connect(reply, &QNetworkReply::encrypted, this,
                [this, requestId] { handleEncrypted(requestId); });
        connect(reply, &QNetworkReply::readyRead, this,
                [this, requestId] { handleReadyRead(requestId); });
        connect(reply, &QNetworkReply::finished, this,
                [this, requestId] { handleFinished(requestId); });

        active->totalTimer->start(
            std::max(1, activeRequest.options.totalTimeoutMs));
        active->idleTimer->start(
            std::max(1, activeRequest.options.idleTimeoutMs));
        m_Active.emplace(requestId, std::move(active));
    }

    void cancel(PolarisApiClient::RequestId requestId)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        auto iterator = m_Active.find(requestId);
        if (iterator == m_Active.end()) {
            return;
        }
        PolarisResponse response;
        response.errorCode = QStringLiteral("cancelled");
        response.userMessage = userMessageForError(response.errorCode);
        response.authenticated = iterator->second->authenticated;
        complete(requestId, std::move(response), true);
    }

    void shutdown()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        m_ShuttingDown = true;
        while (!m_Active.empty()) {
            auto iterator = m_Active.begin();
            ActiveRequest* active = iterator->second.get();
            disconnect(active->reply, nullptr, this, nullptr);
            active->totalTimer->stop();
            active->idleTimer->stop();
            active->reply->abort();
            delete active->reply;
            delete active->totalTimer;
            delete active->idleTimer;
            m_Active.erase(iterator);
        }
        delete m_Manager;
        m_Manager = nullptr;
    }

private:
    struct ActiveRequest
    {
        ~ActiveRequest()
        {
            if (request.sensitiveResponseBody) {
                wipeOwnedBytes(body, request.observer);
            }
        }

        WorkerRequest request;
        QNetworkReply* reply = nullptr;
        QTimer* totalTimer = nullptr;
        QTimer* idleTimer = nullptr;
        QElapsedTimer elapsed;
        QByteArray body;
        bool authenticated = false;
    };

    ActiveRequest* find(PolarisApiClient::RequestId requestId)
    {
        auto iterator = m_Active.find(requestId);
        return iterator == m_Active.end() ? nullptr : iterator->second.get();
    }

    void handleSslErrors(PolarisApiClient::RequestId requestId,
                         const QList<QSslError>& errors)
    {
        ActiveRequest* active = find(requestId);
        if (active == nullptr) {
            return;
        }
        const QSslCertificate peer =
            m_Backend->peerLeafCertificate(active->reply);
        if (PolarisApiClient::acceptsExactPairedIdentity(
                m_ExpectedLeaf, peer, errors)) {
            active->reply->ignoreSslErrors(errors);
            return;
        }
        PolarisResponse response;
        response.errorCode = QStringLiteral("tls_identity_mismatch");
        response.userMessage = userMessageForError(response.errorCode);
        complete(requestId, std::move(response), true);
    }

    void handleEncrypted(PolarisApiClient::RequestId requestId)
    {
        ActiveRequest* active = find(requestId);
        if (active == nullptr) {
            return;
        }
        const QSslCertificate peer =
            m_Backend->peerLeafCertificate(active->reply);
        if (!PolarisApiClient::acceptsExactPairedIdentity(
                m_ExpectedLeaf, peer)) {
            PolarisResponse response;
            response.errorCode = QStringLiteral("tls_identity_mismatch");
            response.userMessage = userMessageForError(response.errorCode);
            complete(requestId, std::move(response), true);
            return;
        }
        active->authenticated = true;
    }

    void handleReadyRead(PolarisApiClient::RequestId requestId)
    {
        ActiveRequest* active = find(requestId);
        if (active == nullptr) {
            return;
        }
        active->idleTimer->start(
            std::max(1, active->request.options.idleTimeoutMs));
        const qint64 limit = std::max<qint64>(
            0, active->request.options.maxBodyBytes);
        const qint64 remaining = limit - active->body.size();
        QByteArray bytes = active->reply->read(remaining + 1);
        ScopedOwnedBytesWipe chunkWipe(
            bytes, active->request.sensitiveResponseBody,
            active->request.observer);
        if (bytes.size() > remaining) {
            if (remaining > 0) {
                active->body.append(bytes.constData(), remaining);
            }
            PolarisResponse response;
            response.httpStatus = active->reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.body = active->body;
            response.authenticated = active->authenticated;
            response.errorCode = QStringLiteral("response_too_large");
            response.userMessage = userMessageForError(response.errorCode);
            complete(requestId, std::move(response), true);
            return;
        }
        active->body.append(bytes);
    }

    void handleFinished(PolarisApiClient::RequestId requestId)
    {
        ActiveRequest* active = find(requestId);
        if (active == nullptr) {
            return;
        }
        handleReadyRead(requestId);
        active = find(requestId);
        if (active == nullptr) {
            return;
        }

        PolarisResponse response;
        response.httpStatus = active->reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = active->body;
        response.authenticated = active->authenticated;
        const QNetworkReply::NetworkError replyError =
            active->reply->error();
        const bool hasUsableHttpStatus =
            response.httpStatus >= 100 && response.httpStatus <= 599;
        if (!active->authenticated &&
                replyError != QNetworkReply::NoError &&
                replyError != QNetworkReply::SslHandshakeFailedError) {
            response.errorCode = QStringLiteral("network_error");
        }
        else if (!active->authenticated) {
            response.errorCode = QStringLiteral("tls_identity_mismatch");
        }
        else if (hasUsableHttpStatus &&
                (response.httpStatus < 200 || response.httpStatus >= 300)) {
            response.errorCode = QStringLiteral("http_error");
        }
        else if (replyError != QNetworkReply::NoError ||
                !hasUsableHttpStatus) {
            response.errorCode = QStringLiteral("network_error");
        }
        else if (active->request.expectJson) {
            QJsonParseError parseError;
            response.json = QJsonDocument::fromJson(response.body, &parseError);
            if (parseError.error != QJsonParseError::NoError ||
                    response.json.isNull()) {
                response.errorCode = QStringLiteral("malformed_json");
            }
        }
        response.userMessage = userMessageForError(response.errorCode);
        complete(requestId, std::move(response), false);
    }

    void timeout(PolarisApiClient::RequestId requestId)
    {
        ActiveRequest* active = find(requestId);
        if (active == nullptr) {
            return;
        }
        PolarisResponse response;
        response.body = active->body;
        response.authenticated = active->authenticated;
        response.errorCode = QStringLiteral("timeout");
        response.userMessage = userMessageForError(response.errorCode);
        complete(requestId, std::move(response), true);
    }

    void complete(PolarisApiClient::RequestId requestId,
                  PolarisResponse response, bool abortReply)
    {
        auto iterator = m_Active.find(requestId);
        if (iterator == m_Active.end()) {
            return;
        }
        ActiveRequest* pending = iterator->second.get();
        const int terminalStatus = response.httpStatus;
        const QString terminalError = response.errorCode;
        const TerminalClaim claim = queueTerminal(
            m_State, requestId, response,
            pending->request.sensitiveResponseBody,
            pending->request.observer);
        if (claim == TerminalClaim::Cancelled) {
            abortReply = true;
        }
        std::unique_ptr<ActiveRequest> active = std::move(iterator->second);
        m_Active.erase(iterator);
        active->totalTimer->stop();
        active->idleTimer->stop();
        disconnect(active->reply, nullptr, this, nullptr);
        if (abortReply && !active->reply->isFinished()) {
            active->reply->abort();
        }
        const int elapsedMs = static_cast<int>(active->elapsed.elapsed());
        active->reply->deleteLater();
        delete active->totalTimer;
        delete active->idleTimer;
        if (claim != TerminalClaim::Rejected) {
            logCompletion(active->request, terminalStatus,
                          terminalError, elapsedMs);
        }
    }

    static void logCompletion(const WorkerRequest& request, int status,
                              const QString& errorCode, int elapsedMs)
    {
        qInfo().noquote()
            << QStringLiteral("Polaris %1 %2 status=%3 elapsed_ms=%4 error=%5")
                   .arg(QString::fromLatin1(request.method),
                        PerigeeRedaction::pathForLog(request.logPath),
                        QString::number(status),
                        QString::number(elapsedMs),
                        errorCode.isEmpty()
                            ? QStringLiteral("none") : errorCode);
    }

    QSslCertificate m_ExpectedLeaf;
    QSslConfiguration m_Identity;
    std::unique_ptr<PolarisNetworkBackend> m_Backend;
    std::shared_ptr<SharedRequestState> m_State;
    QNetworkAccessManager* m_Manager = nullptr;
    std::unordered_map<PolarisApiClient::RequestId,
                       std::unique_ptr<ActiveRequest>> m_Active;
    bool m_ShuttingDown = false;
};
}

struct PolarisApiClient::Private
{
    QUrl origin;
    std::shared_ptr<SharedRequestState> state =
        std::make_shared<SharedRequestState>();
    QThread thread;
    PolarisNetworkWorker* worker = nullptr;
    std::atomic<RequestId> nextRequestId = 1;
};

PolarisApiClient::PolarisApiClient(
    const NvComputer& computer,
    std::unique_ptr<PolarisNetworkBackend> backend)
    : d(std::make_unique<Private>())
{
    initialize(computer, IdentityManager::get()->getSslConfig(),
               std::move(backend));
}

#ifdef PERIGEE_TESTING
PolarisApiClient::PolarisApiClient(
    const NvComputer& computer, TestIdentityTag,
    const QSslConfiguration& testIdentity,
    std::unique_ptr<PolarisNetworkBackend> backend)
    : d(std::make_unique<Private>())
{
    initialize(computer, testIdentity, std::move(backend));
}
#endif

void PolarisApiClient::initialize(
    const NvComputer& computer, QSslConfiguration identity,
    std::unique_ptr<PolarisNetworkBackend> backend)
{
    NvAddress activeAddress;
    quint16 activeHttpsPort = 0;
    QSslCertificate serverCert;
    {
        QReadLocker locker(&computer.lock);
        activeAddress = computer.activeAddress;
        activeHttpsPort = computer.activeHttpsPort;
        serverCert = computer.serverCert;
    }
    d->origin = pairedOrigin(activeAddress, activeHttpsPort);
    if (!backend) {
        backend = std::make_unique<QtPolarisNetworkBackend>();
    }
    d->worker = new PolarisNetworkWorker(
        std::move(serverCert), std::move(identity),
        std::move(backend), d->state);
    d->worker->moveToThread(&d->thread);
    d->thread.setObjectName(QStringLiteral("PolarisApiClientNetwork"));
    d->thread.start();
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker] { worker->initialize(); },
        Qt::BlockingQueuedConnection);
}

PolarisApiClient::~PolarisApiClient()
{
    const std::shared_ptr<SharedRequestState> state = d->state;
    std::deque<CompletionEntry> discardedCompletions;
    {
        QMutexLocker locker(&state->mutex);
        state->accepting = false;
        discardedCompletions = std::move(state->queue);
        state->requests.clear();
    }
    discardedCompletions.clear();
    if (d->worker != nullptr && d->thread.isRunning()) {
        PolarisNetworkWorker* worker = d->worker;
        QMetaObject::invokeMethod(worker,
            [worker] {
                worker->shutdown();
                delete worker;
            }, Qt::BlockingQueuedConnection);
        d->worker = nullptr;
        d->thread.quit();
        d->thread.wait();
    }
}

PolarisApiClient::RequestId PolarisApiClient::get(
    const QString& endpoint, bool expectJson, Completion completion,
    PolarisRequestOptions options)
{
    return request(QByteArrayLiteral("GET"), endpoint, {}, expectJson,
                   std::move(completion), options);
}

PolarisApiClient::RequestId PolarisApiClient::request(
    const QByteArray& method, const QString& endpoint,
    const QByteArray& body, bool expectJson, Completion completion,
    PolarisRequestOptions options)
{
    const RequestId requestId =
        d->nextRequestId.fetch_add(1, std::memory_order_relaxed);

    const QByteArray normalizedMethod = method.toUpper();
    QString errorCode;
    QUrl url;
    if (normalizedMethod != QByteArrayLiteral("GET") &&
            normalizedMethod != QByteArrayLiteral("POST")) {
        errorCode = QStringLiteral("endpoint_policy_rejected");
    }
    else if (normalizedMethod == QByteArrayLiteral("GET") &&
            !body.isEmpty()) {
        errorCode = QStringLiteral("endpoint_policy_rejected");
    }
    else {
        url = resolveAdvertisedEndpoint(d->origin, endpoint, &errorCode);
    }
    const std::shared_ptr<SharedRequestState> state = d->state;
    if (url.isEmpty()) {
        PolarisResponse response;
        response.errorCode = errorCode;
        response.userMessage = userMessageForError(errorCode);
        QMutexLocker locker(&state->mutex);
        if (state->accepting) {
            state->requests.insert(requestId,
                RequestRecord{std::move(completion),
                              RequestPhase::TerminalQueued});
            state->queue.emplace_back(requestId, std::move(response),
                                      false, WipeObserver());
        }
        return requestId;
    }

    {
        QMutexLocker locker(&state->mutex);
        if (!state->accepting) {
            return requestId;
        }
        state->requests.insert(requestId,
            RequestRecord{std::move(completion), RequestPhase::Submitted});
    }
    options.maxBodyBytes = std::clamp<qint64>(
        options.maxBodyBytes, 0, kAbsoluteBodyLimit);
    auto workerRequest = std::make_shared<WorkerRequest>(
        requestId, normalizedMethod, url, endpoint, body,
        QStringLiteral("application/json"), expectJson, options,
        WipeObserver(), !body.isEmpty(), false);
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, workerRequest]() mutable {
            worker->startRequest(std::move(*workerRequest));
        }, Qt::QueuedConnection);
    return requestId;
}

PolarisApiClient::RequestId PolarisApiClient::fetchClipboard(
    std::optional<qint64> advertisedLimit, Completion completion,
    PolarisRequestOptions options)
{
    const qint64 hostLimit = advertisedLimit.has_value() &&
            *advertisedLimit >= 0 ? *advertisedLimit : kAbsoluteBodyLimit;
    options.maxBodyBytes = std::min(hostLimit, kAbsoluteBodyLimit);

    const RequestId requestId =
        d->nextRequestId.fetch_add(1, std::memory_order_relaxed);
    const std::shared_ptr<SharedRequestState> state = d->state;
    if (d->origin.isEmpty()) {
        PolarisResponse response;
        response.errorCode = QStringLiteral("endpoint_policy_rejected");
        response.userMessage = userMessageForError(response.errorCode);
        QMutexLocker locker(&state->mutex);
        if (state->accepting) {
            state->requests.insert(requestId,
                RequestRecord{std::move(completion),
                              RequestPhase::TerminalQueued});
            state->queue.emplace_back(requestId, std::move(response),
                                      false, WipeObserver());
        }
        return requestId;
    }

    {
        QMutexLocker locker(&state->mutex);
        if (!state->accepting) {
            return requestId;
        }
        state->requests.insert(requestId,
            RequestRecord{std::move(completion), RequestPhase::Submitted});
    }
    QUrl url = d->origin;
    url.setPath(QStringLiteral("/actions/clipboard"));
    url.setQuery(QStringLiteral("type=text"));
    auto workerRequest = std::make_shared<WorkerRequest>(
        requestId, QByteArrayLiteral("GET"), url,
        QStringLiteral("/actions/clipboard?type=text"), QByteArray(),
        QStringLiteral("text/plain; charset=utf-8"), false, options,
        WipeObserver(), false, true);
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, workerRequest]() mutable {
            worker->startRequest(std::move(*workerRequest));
        }, Qt::QueuedConnection);
    return requestId;
}

PolarisApiClient::RequestId PolarisApiClient::sendClipboard(
    const QByteArray& body, Completion completion,
    PolarisRequestOptions options)
{
    const RequestId requestId =
        d->nextRequestId.fetch_add(1, std::memory_order_relaxed);
    const std::shared_ptr<SharedRequestState> state = d->state;
    QString errorCode;
    if (d->origin.isEmpty()) {
        errorCode = QStringLiteral("endpoint_policy_rejected");
    }
    else if (body.size() > kAbsoluteBodyLimit) {
        errorCode = QStringLiteral("request_too_large");
    }
    if (!errorCode.isEmpty()) {
        PolarisResponse response;
        response.errorCode = errorCode;
        response.userMessage = userMessageForError(errorCode);
        QMutexLocker locker(&state->mutex);
        if (state->accepting) {
            state->requests.insert(
                requestId,
                RequestRecord{std::move(completion),
                              RequestPhase::TerminalQueued});
            state->queue.emplace_back(requestId, std::move(response),
                                      false, WipeObserver());
        }
        return requestId;
    }

    {
        QMutexLocker locker(&state->mutex);
        if (!state->accepting) {
            return requestId;
        }
        state->requests.insert(
            requestId,
            RequestRecord{std::move(completion), RequestPhase::Submitted});
    }
    options.maxBodyBytes = std::clamp<qint64>(
        options.maxBodyBytes, 0, kAbsoluteBodyLimit);
    QUrl url = d->origin;
    url.setPath(QStringLiteral("/actions/clipboard"));
    url.setQuery(QStringLiteral("type=text"));
    auto workerRequest = std::make_shared<WorkerRequest>(
        requestId, QByteArrayLiteral("POST"), url,
        QStringLiteral("/actions/clipboard?type=text"), body,
        QStringLiteral("text/plain; charset=utf-8"), false, options,
        WipeObserver(), true, false);
    QMetaObject::invokeMethod(
        d->worker,
        [worker = d->worker,
         workerRequest]() mutable {
            worker->startRequest(std::move(*workerRequest));
        },
        Qt::QueuedConnection);
    return requestId;
}

bool PolarisApiClient::cancel(RequestId requestId)
{
    const std::shared_ptr<SharedRequestState> state = d->state;
    {
        QMutexLocker locker(&state->mutex);
        auto iterator = state->requests.find(requestId);
        if (!state->accepting || iterator == state->requests.end() ||
                iterator->phase != RequestPhase::Submitted) {
            return false;
        }
        iterator->phase = RequestPhase::CancelRequested;
    }
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, requestId] { worker->cancel(requestId); },
        Qt::QueuedConnection);
    return true;
}

int PolarisApiClient::drainCompletions(int maximum)
{
    if (maximum <= 0) {
        return 0;
    }
    struct DispatchEntry
    {
        CompletionEntry completion;
        Completion callback;
    };
    const std::shared_ptr<SharedRequestState> state = d->state;
    std::vector<DispatchEntry> ready;
    {
        QMutexLocker locker(&state->mutex);
        if (!state->accepting || state->draining) {
            return 0;
        }
        state->draining = true;
        const int count = std::min(
            maximum, static_cast<int>(state->queue.size()));
        ready.reserve(count);
        for (int i = 0; i < count; ++i) {
            CompletionEntry completion = std::move(state->queue.front());
            state->queue.pop_front();
            auto iterator = state->requests.find(completion.requestId);
            if (iterator == state->requests.end() ||
                    iterator->phase != RequestPhase::TerminalQueued) {
                ready.push_back(DispatchEntry{
                    std::move(completion), Completion()});
                continue;
            }
            Completion callback = std::move(iterator->callback);
            state->requests.erase(iterator);
            ready.push_back(DispatchEntry{
                std::move(completion), std::move(callback)});
        }
    }

    struct DrainGuard
    {
        std::shared_ptr<SharedRequestState> state;
        ~DrainGuard()
        {
            QMutexLocker locker(&state->mutex);
            state->draining = false;
        }
    } guard{state};
    const int drained = ready.size();
    for (const DispatchEntry& entry : ready) {
        {
            QMutexLocker locker(&state->mutex);
            if (!state->accepting) {
                break;
            }
        }
        if (entry.callback) {
            try {
                entry.callback(entry.completion.requestId,
                               entry.completion.response);
            }
            catch (...) {
                qWarning().noquote()
                    << QStringLiteral(
                        "Polaris completion callback failed; continuing");
            }
        }
    }
    return drained;
}

qsizetype PolarisApiClient::pendingCompletionCount() const
{
    const std::shared_ptr<SharedRequestState> state = d->state;
    QMutexLocker locker(&state->mutex);
    return state->queue.size();
}

QUrl PolarisApiClient::origin() const
{
    return d != nullptr ? d->origin : QUrl();
}

QUrl PolarisApiClient::pairedOrigin(const NvAddress& address,
                                    quint16 httpsPort)
{
    if (address.isNull() || httpsPort == 0) {
        return {};
    }
    QUrl origin;
    origin.setScheme(QStringLiteral("https"));
    origin.setHost(address.address());
    origin.setPort(httpsPort);
    return origin;
}

QUrl PolarisApiClient::resolveAdvertisedEndpoint(const QUrl& pairedOrigin,
                                                 const QString& endpoint,
                                                 QString* errorCode)
{
    if (errorCode != nullptr) {
        errorCode->clear();
    }
    if (!pairedOrigin.isValid() ||
            pairedOrigin.scheme() != QStringLiteral("https") ||
            pairedOrigin.host().isEmpty() || pairedOrigin.port() <= 0 ||
            !pairedOrigin.userInfo().isEmpty() || pairedOrigin.hasFragment() ||
            endpoint.isEmpty() || endpoint.size() > 4096 ||
            !endpoint.startsWith(QStringLiteral("/polaris/v1/")) ||
            endpoint.contains(QLatin1Char('#')) ||
            endpoint.contains(QLatin1Char('\\')) ||
            hasControlCharacter(endpoint) ||
            hasMalformedPercentEscape(endpoint)) {
        return rejectEndpoint(errorCode);
    }

    const int queryIndex = endpoint.indexOf(QLatin1Char('?'));
    const QString rawPath = queryIndex < 0
        ? endpoint : endpoint.left(queryIndex);
    if (rawPath.contains(QLatin1Char('%')) ||
            rawPath.contains(QStringLiteral("//"))) {
        return rejectEndpoint(errorCode);
    }
    const QStringList pathSegments = rawPath.split(QLatin1Char('/'));
    if (pathSegments.contains(QStringLiteral(".")) ||
            pathSegments.contains(QStringLiteral(".."))) {
        return rejectEndpoint(errorCode);
    }

    const QUrl relative(endpoint, QUrl::StrictMode);
    if (!relative.isValid() || !relative.isRelative() ||
            !relative.scheme().isEmpty() || !relative.authority().isEmpty() ||
            !relative.userInfo().isEmpty() || !relative.host().isEmpty() ||
            relative.port(-1) != -1 || relative.hasFragment() ||
            relative.path(QUrl::FullyEncoded) != rawPath) {
        return rejectEndpoint(errorCode);
    }

    QUrl resolved = pairedOrigin;
    resolved.setPath(relative.path());
    resolved.setQuery(relative.query(QUrl::FullyEncoded));
    if (resolved.scheme() != pairedOrigin.scheme() ||
            resolved.host() != pairedOrigin.host() ||
            resolved.port() != pairedOrigin.port()) {
        return rejectEndpoint(errorCode);
    }
    return resolved;
}

QNetworkRequest PolarisApiClient::makeRequest(
    const QUrl& url, const QSslConfiguration& identity)
{
    QNetworkRequest request(url);
    request.setSslConfiguration(identity);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

bool PolarisApiClient::acceptsExactPairedIdentity(
    const QSslCertificate& expectedLeaf,
    const QSslCertificate& peerLeaf,
    const QList<QSslError>& errors)
{
    if (expectedLeaf.isNull() || peerLeaf.isNull() ||
            expectedLeaf != peerLeaf) {
        return false;
    }
    for (const QSslError& error : errors) {
        if (error.certificate().isNull() ||
                error.certificate() != expectedLeaf) {
            return false;
        }
    }
    return true;
}
