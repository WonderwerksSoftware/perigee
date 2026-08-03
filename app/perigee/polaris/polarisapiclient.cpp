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
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace
{
constexpr qint64 kAbsoluteBodyLimit = 1024 * 1024;

struct CompletionEntry
{
    PolarisApiClient::RequestId requestId = 0;
    PolarisResponse response;
};

struct SharedCompletions
{
    mutable QMutex mutex;
    QQueue<CompletionEntry> queue;
    std::atomic_bool accepting = true;
};

struct WorkerRequest
{
    PolarisApiClient::RequestId requestId = 0;
    QByteArray method;
    QUrl url;
    QString logPath;
    QByteArray body;
    bool expectJson = false;
    PolarisRequestOptions options;
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
        std::shared_ptr<SharedCompletions> completions)
        : m_ExpectedLeaf(std::move(expectedLeaf))
        , m_Identity(std::move(identity))
        , m_Backend(std::move(backend))
        , m_Completions(std::move(completions))
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
        if (m_ShuttingDown ||
                !m_Completions->accepting.load(std::memory_order_acquire)) {
            return;
        }
        if (m_ExpectedLeaf.isNull()) {
            PolarisResponse response;
            response.errorCode = QStringLiteral("tls_identity_mismatch");
            response.userMessage = userMessageForError(response.errorCode);
            enqueueCompletion(request.requestId, std::move(response));
            logCompletion(request, 0,
                          QStringLiteral("tls_identity_mismatch"), 0);
            return;
        }

        auto active = std::make_unique<ActiveRequest>();
        active->request = request;
        active->elapsed.start();
        QNetworkRequest networkRequest =
            PolarisApiClient::makeRequest(request.url, m_Identity);
        if (request.method == QByteArrayLiteral("GET")) {
            active->reply = m_Manager->get(networkRequest);
        }
        else {
            networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                     QStringLiteral("application/json"));
            active->reply = m_Manager->sendCustomRequest(
                networkRequest, request.method, request.body);
        }
        QNetworkReply* reply = active->reply;
        const auto requestId = request.requestId;

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

        active->totalTimer->start(std::max(1, request.options.totalTimeoutMs));
        active->idleTimer->start(std::max(1, request.options.idleTimeoutMs));
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
        const QByteArray bytes = active->reply->read(remaining + 1);
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
        if (!active->authenticated) {
            response.errorCode = QStringLiteral("tls_identity_mismatch");
        }
        else if (active->reply->error() != QNetworkReply::NoError) {
            response.errorCode = QStringLiteral("network_error");
        }
        else if (response.httpStatus < 200 || response.httpStatus >= 300) {
            response.errorCode = QStringLiteral("http_error");
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
        std::unique_ptr<ActiveRequest> active = std::move(iterator->second);
        m_Active.erase(iterator);
        active->totalTimer->stop();
        active->idleTimer->stop();
        disconnect(active->reply, nullptr, this, nullptr);
        if (abortReply && !active->reply->isFinished()) {
            active->reply->abort();
        }
        const int elapsedMs = static_cast<int>(active->elapsed.elapsed());
        const WorkerRequest request = active->request;
        active->reply->deleteLater();
        delete active->totalTimer;
        delete active->idleTimer;
        logCompletion(request, response.httpStatus,
                      response.errorCode, elapsedMs);
        enqueueCompletion(requestId, std::move(response));
    }

    void enqueueCompletion(PolarisApiClient::RequestId requestId,
                           PolarisResponse response)
    {
        if (!m_Completions->accepting.load(std::memory_order_acquire)) {
            return;
        }
        QMutexLocker locker(&m_Completions->mutex);
        if (m_Completions->accepting.load(std::memory_order_relaxed)) {
            m_Completions->queue.enqueue(
                CompletionEntry{requestId, std::move(response)});
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
    std::shared_ptr<SharedCompletions> m_Completions;
    QNetworkAccessManager* m_Manager = nullptr;
    std::unordered_map<PolarisApiClient::RequestId,
                       std::unique_ptr<ActiveRequest>> m_Active;
    bool m_ShuttingDown = false;
};
}

struct PolarisApiClient::Private
{
    QUrl origin;
    std::shared_ptr<SharedCompletions> completions =
        std::make_shared<SharedCompletions>();
    QThread thread;
    PolarisNetworkWorker* worker = nullptr;
    mutable QMutex callbacksMutex;
    QHash<RequestId, Completion> callbacks;
    QSet<RequestId> cancelRequested;
    std::atomic<RequestId> nextRequestId = 1;
};

PolarisApiClient::PolarisApiClient(
    const NvComputer& computer,
    std::unique_ptr<PolarisNetworkBackend> backend)
    : d(std::make_unique<Private>())
{
    d->origin = pairedOrigin(computer.activeAddress,
                             computer.activeHttpsPort);
    QSslConfiguration identity = IdentityManager::get()->getSslConfig();
    if (!backend) {
        backend = std::make_unique<QtPolarisNetworkBackend>();
    }
    d->worker = new PolarisNetworkWorker(
        computer.serverCert, std::move(identity),
        std::move(backend), d->completions);
    d->worker->moveToThread(&d->thread);
    d->thread.setObjectName(QStringLiteral("PolarisApiClientNetwork"));
    d->thread.start();
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker] { worker->initialize(); },
        Qt::BlockingQueuedConnection);
}

PolarisApiClient::~PolarisApiClient()
{
    d->completions->accepting.store(false, std::memory_order_release);
    {
        QMutexLocker locker(&d->callbacksMutex);
        d->callbacks.clear();
        d->cancelRequested.clear();
    }
    {
        QMutexLocker locker(&d->completions->mutex);
        d->completions->queue.clear();
    }
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
    {
        QMutexLocker locker(&d->callbacksMutex);
        d->callbacks.insert(requestId, std::move(completion));
    }

    const QByteArray normalizedMethod = method.toUpper();
    QString errorCode;
    QUrl url;
    if (normalizedMethod != QByteArrayLiteral("GET") &&
            normalizedMethod != QByteArrayLiteral("POST")) {
        errorCode = QStringLiteral("endpoint_policy_rejected");
    }
    else {
        url = resolveAdvertisedEndpoint(d->origin, endpoint, &errorCode);
    }
    if (url.isEmpty()) {
        PolarisResponse response;
        response.errorCode = errorCode;
        response.userMessage = userMessageForError(errorCode);
        QMutexLocker locker(&d->completions->mutex);
        d->completions->queue.enqueue(
            CompletionEntry{requestId, std::move(response)});
        return requestId;
    }

    options.maxBodyBytes = std::clamp<qint64>(
        options.maxBodyBytes, 0, kAbsoluteBodyLimit);
    WorkerRequest workerRequest{requestId, normalizedMethod, url,
                                endpoint, body, expectJson, options};
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, workerRequest = std::move(workerRequest)]() mutable {
            worker->startRequest(std::move(workerRequest));
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
    {
        QMutexLocker locker(&d->callbacksMutex);
        d->callbacks.insert(requestId, std::move(completion));
    }
    if (d->origin.isEmpty()) {
        PolarisResponse response;
        response.errorCode = QStringLiteral("endpoint_policy_rejected");
        response.userMessage = userMessageForError(response.errorCode);
        QMutexLocker locker(&d->completions->mutex);
        d->completions->queue.enqueue(
            CompletionEntry{requestId, std::move(response)});
        return requestId;
    }

    QUrl url = d->origin;
    url.setPath(QStringLiteral("/actions/clipboard"));
    WorkerRequest workerRequest{
        requestId, QByteArrayLiteral("GET"), url,
        QStringLiteral("/actions/clipboard"), {}, false, options};
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, workerRequest = std::move(workerRequest)]() mutable {
            worker->startRequest(std::move(workerRequest));
        }, Qt::QueuedConnection);
    return requestId;
}

bool PolarisApiClient::cancel(RequestId requestId)
{
    {
        QMutexLocker locker(&d->callbacksMutex);
        if (!d->callbacks.contains(requestId) ||
                d->cancelRequested.contains(requestId)) {
            return false;
        }
        d->cancelRequested.insert(requestId);
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
    QList<CompletionEntry> ready;
    {
        QMutexLocker locker(&d->completions->mutex);
        const int count = std::min(
            maximum, static_cast<int>(d->completions->queue.size()));
        ready.reserve(count);
        for (int i = 0; i < count; ++i) {
            ready.append(d->completions->queue.dequeue());
        }
    }

    for (const CompletionEntry& entry : ready) {
        Completion callback;
        {
            QMutexLocker locker(&d->callbacksMutex);
            callback = d->callbacks.take(entry.requestId);
            d->cancelRequested.remove(entry.requestId);
        }
        if (callback) {
            callback(entry.requestId, entry.response);
        }
    }
    return ready.size();
}

qsizetype PolarisApiClient::pendingCompletionCount() const
{
    QMutexLocker locker(&d->completions->mutex);
    return d->completions->queue.size();
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
    if (rawPath.contains(QLatin1Char('%'))) {
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
