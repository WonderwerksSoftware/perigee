#include "fakepolarisserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace
{
constexpr qsizetype MaximumRequestLineBytes = 4096;
constexpr qsizetype MaximumHeaderBytes = 64 * 1024;
constexpr qsizetype MaximumBodyBytes = 1024 * 1024;
constexpr qint64 MaximumSocketReadBufferBytes = MaximumBodyBytes + 1;

bool hasInvalidHeaderLineEnding(const QByteArray& bytes)
{
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        if (bytes.at(index) == '\n' &&
                (index == 0 || bytes.at(index - 1) != '\r')) {
            return true;
        }
        if (bytes.at(index) == '\r' && index + 1 < bytes.size() &&
                bytes.at(index + 1) != '\n') {
            return true;
        }
    }
    return false;
}

bool isHttpToken(const QByteArray& value)
{
    if (value.isEmpty()) {
        return false;
    }
    for (const unsigned char byte : value) {
        const bool alphaNumeric =
            (byte >= '0' && byte <= '9') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z');
        const bool punctuation =
            byte == '!' || byte == '#' || byte == '$' || byte == '%' ||
            byte == '&' || byte == '\'' || byte == '*' || byte == '+' ||
            byte == '-' || byte == '.' || byte == '^' || byte == '_' ||
            byte == '`' || byte == '|' || byte == '~';
        if (!alphaNumeric && !punctuation) {
            return false;
        }
    }
    return true;
}

void wipeBytes(QByteArray& bytes, int* wipeCount = nullptr,
               bool* allBytesWereZero = nullptr,
               qsizetype* logicalBytesWiped = nullptr)
{
    const qsizetype logicalSize = bytes.size();
    if (bytes.capacity() > bytes.size()) {
        bytes.resize(bytes.capacity());
    }
    volatile unsigned char* data =
        reinterpret_cast<volatile unsigned char*>(bytes.data());
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        data[index] = 0;
    }
    if (wipeCount != nullptr && !bytes.isEmpty()) {
        ++*wipeCount;
    }
    if (logicalBytesWiped != nullptr) {
        *logicalBytesWiped += logicalSize;
    }
    if (allBytesWereZero != nullptr) {
        for (char byte : std::as_const(bytes)) {
            *allBytesWereZero &= byte == '\0';
        }
    }
    bytes.clear();
    bytes.squeeze();
}

QString fixtureDirectory()
{
    const QFileInfo source(QString::fromUtf8(__FILE__));
    return QDir(source.absolutePath()).absoluteFilePath(
        QStringLiteral("../fixtures/tls"));
}

QByteArray readFixture(const QString& name)
{
    QFile file(QDir(fixtureDirectory()).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QSslCertificate readCertificate(const QString& name)
{
    const QList<QSslCertificate> certificates =
        QSslCertificate::fromData(readFixture(name), QSsl::Pem);
    return certificates.size() == 1
        ? certificates.constFirst() : QSslCertificate();
}

QSslKey readPrivateKey(const QString& name)
{
    QByteArray pem = readFixture(name);
    QSslKey key(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    wipeBytes(pem);
    return key;
}

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200: return QByteArrayLiteral("OK");
    case 204: return QByteArrayLiteral("No Content");
    case 400: return QByteArrayLiteral("Bad Request");
    case 401: return QByteArrayLiteral("Unauthorized");
    case 403: return QByteArrayLiteral("Forbidden");
    case 404: return QByteArrayLiteral("Not Found");
    case 409: return QByteArrayLiteral("Conflict");
    case 470: return QByteArrayLiteral("Permission Denied");
    case 500: return QByteArrayLiteral("Internal Server Error");
    default: return QByteArrayLiteral("Scripted");
    }
}

QString normalizeTarget(const QByteArray& target)
{
    if (target.isEmpty() || !target.startsWith('/')) {
        return {};
    }
    const QUrl url(QString::fromLatin1(target), QUrl::StrictMode);
    if (!url.isValid() || !url.isRelative() || url.hasFragment() ||
            url.path().isEmpty()) {
        return {};
    }
    QString normalized = url.path(QUrl::FullyEncoded);
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) {
        normalized += QLatin1Char('?');
        normalized += query;
    }
    return normalized;
}
}

class FakePolarisServer::Listener final : public QTcpServer
{
public:
    explicit Listener(FakePolarisServer* owner)
        : QTcpServer(owner), m_Owner(owner) {}

protected:
    void incomingConnection(qintptr descriptor) override
    {
        m_Owner->acceptDescriptor(descriptor);
    }

private:
    FakePolarisServer* m_Owner;
};

struct FakePolarisServer::Connection
{
    ~Connection()
    {
        wipeBytes(input);
        wipeBytes(body);
    }

    QPointer<QSslSocket> socket;
    QByteArray input;
    QByteArray body;
    QByteArray method;
    QString path;
    qsizetype contentLength = -1;
    bool headersParsed = false;
    bool authenticated = false;
    bool handled = false;
    bool pendingDelete = false;
};

struct FakePolarisServer::ResponseWriteState
{
    ResponseWriteState(std::shared_ptr<ResponseWipeAudit> responseAudit,
                       QSslSocket* responseSocket,
                       ResponseScript responseScript)
        : audit(std::move(responseAudit))
        , socket(responseSocket)
        , script(std::move(responseScript))
    {
    }

    ~ResponseWriteState()
    {
        for (QByteArray& chunk : script.bodyChunks) {
            wipeBytes(chunk, &audit->wipeCount, &audit->allBytesWereZero,
                      &audit->logicalBytesWiped);
        }
        wipeBytes(script.malformedResponse, &audit->wipeCount,
                  &audit->allBytesWereZero,
                  &audit->logicalBytesWiped);
    }

    std::shared_ptr<ResponseWipeAudit> audit;
    QPointer<QSslSocket> socket;
    ResponseScript script;
};

FakePolarisServer::FakePolarisServer(QObject* parent)
    : QObject(parent)
    , m_Listener(std::make_unique<Listener>(this))
    , m_ResponseWipeAudit(std::make_shared<ResponseWipeAudit>())
{
}

FakePolarisServer::~FakePolarisServer()
{
    stop();
}

bool FakePolarisServer::loadFixtures()
{
    m_CaCertificate = readCertificate(QStringLiteral("ca.crt"));
    m_ServerCertificate = readCertificate(QStringLiteral("server.crt"));
    m_ClientCertificate = readCertificate(QStringLiteral("client.crt"));
    const QSslKey serverKey = readPrivateKey(QStringLiteral("server.key"));
    const QSslKey clientKey = readPrivateKey(QStringLiteral("client.key"));
    if (m_CaCertificate.isNull() || m_ServerCertificate.isNull() ||
            m_ClientCertificate.isNull() || serverKey.isNull() ||
            clientKey.isNull()) {
        m_FailureLabel = QStringLiteral("fixture_load_failed");
        return false;
    }

    m_ServerConfiguration = QSslConfiguration::defaultConfiguration();
    m_ServerConfiguration.setLocalCertificateChain(
        {m_ServerCertificate, m_CaCertificate});
    m_ServerConfiguration.setPrivateKey(serverKey);
    m_ServerConfiguration.setCaCertificates({m_CaCertificate});
    m_ServerConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_ServerConfiguration.setProtocol(QSsl::SecureProtocols);

    m_ClientConfiguration = QSslConfiguration::defaultConfiguration();
    m_ClientConfiguration.setLocalCertificateChain(
        {m_ClientCertificate, m_CaCertificate});
    m_ClientConfiguration.setPrivateKey(clientKey);
    m_ClientConfiguration.setCaCertificates({m_CaCertificate});
    m_ClientConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_ClientConfiguration.setProtocol(QSsl::SecureProtocols);

    // This valid CA-signed server identity is intentionally unusable as a
    // client identity. It has the wrong leaf and server-auth-only usage.
    m_WrongClientConfiguration = QSslConfiguration::defaultConfiguration();
    m_WrongClientConfiguration.setLocalCertificateChain(
        {m_ServerCertificate, m_CaCertificate});
    m_WrongClientConfiguration.setPrivateKey(serverKey);
    m_WrongClientConfiguration.setCaCertificates({m_CaCertificate});
    m_WrongClientConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_WrongClientConfiguration.setProtocol(QSsl::SecureProtocols);
    return true;
}

bool FakePolarisServer::start()
{
    if (isListening()) {
        return true;
    }
    m_FailureLabel.clear();
    if (!loadFixtures()) {
        return false;
    }
    if (!m_Listener->listen(QHostAddress::LocalHost, 0)) {
        m_FailureLabel = QStringLiteral("loopback_listen_failed");
        return false;
    }
    if (m_Listener->serverAddress() != QHostAddress(QHostAddress::LocalHost) ||
            m_Listener->serverPort() == 0) {
        m_Listener->close();
        m_FailureLabel = QStringLiteral("unsafe_listener_address");
        return false;
    }
    return true;
}

bool FakePolarisServer::stop()
{
    if (m_Listener) {
        m_Listener->close();
    }
    for (const auto& state : m_Connections) {
        if (state->socket != nullptr) {
            state->socket->disconnect(this);
            state->socket->abort();
            delete state->socket;
        }
    }
    m_Connections.clear();
    while (!m_Scripts.isEmpty()) {
        ResponseScript script = m_Scripts.dequeue();
        for (QByteArray& chunk : script.bodyChunks) {
            wipeBytes(chunk);
        }
        wipeBytes(script.malformedResponse);
    }
    return !isListening() && m_Connections.empty();
}

bool FakePolarisServer::isListening() const
{
    return m_Listener && m_Listener->isListening();
}

QHostAddress FakePolarisServer::address() const
{
    return m_Listener ? m_Listener->serverAddress() : QHostAddress();
}

quint16 FakePolarisServer::port() const
{
    return m_Listener ? m_Listener->serverPort() : 0;
}

QString FakePolarisServer::failureLabel() const
{
    return m_FailureLabel;
}

void FakePolarisServer::enqueue(ResponseScript script)
{
    m_Scripts.enqueue(std::move(script));
}

QList<FakePolarisServer::RequestRecord>
FakePolarisServer::requestHistory() const
{
    return m_History;
}

int FakePolarisServer::unexpectedRequestCount() const
{
    return m_UnexpectedRequests;
}

int FakePolarisServer::rejectedTlsConnectionCount() const
{
    return m_RejectedTlsConnections;
}

int FakePolarisServer::requestBodyWipeCount() const
{
    return m_RequestBodyWipes;
}

bool FakePolarisServer::allRequestBodyWipesWereZero() const
{
    return m_AllRequestBodyWipesWereZero;
}

int FakePolarisServer::responseBodyWipeCount() const
{
    return m_ResponseWipeAudit->wipeCount;
}

bool FakePolarisServer::allResponseBodyWipesWereZero() const
{
    return m_ResponseWipeAudit->allBytesWereZero;
}

std::shared_ptr<const FakePolarisServer::ResponseWipeAudit>
FakePolarisServer::responseWipeAudit() const
{
    return m_ResponseWipeAudit;
}

qsizetype FakePolarisServer::pendingDeleteConnectionCount() const
{
    return std::count_if(
        m_Connections.cbegin(), m_Connections.cend(),
        [](const std::unique_ptr<Connection>& state) {
            return state->pendingDelete;
        });
}

qsizetype FakePolarisServer::synchronousTeardownSocketCount() const
{
    return std::count_if(
        m_Connections.cbegin(), m_Connections.cend(),
        [](const std::unique_ptr<Connection>& state) {
            return state->socket != nullptr;
        });
}

void FakePolarisServer::setPendingDeleteObserver(
    std::function<void()> observer)
{
    m_PendingDeleteObserver = std::move(observer);
}

qsizetype FakePolarisServer::pendingScriptCount() const
{
    return m_Scripts.size();
}

bool FakePolarisServer::waitForRequestCount(qsizetype count, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (m_History.size() < count && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return m_History.size() >= count;
}

QSslCertificate FakePolarisServer::serverCertificate() const
{
    return m_ServerCertificate;
}

QSslCertificate FakePolarisServer::clientCertificate() const
{
    return m_ClientCertificate;
}

QSslConfiguration FakePolarisServer::clientSslConfiguration() const
{
    return m_ClientConfiguration;
}

QSslConfiguration FakePolarisServer::emptyClientSslConfiguration() const
{
    QSslConfiguration configuration =
        QSslConfiguration::defaultConfiguration();
    configuration.setCaCertificates({m_CaCertificate});
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    configuration.setProtocol(QSsl::SecureProtocols);
    return configuration;
}

QSslConfiguration FakePolarisServer::wrongClientSslConfiguration() const
{
    return m_WrongClientConfiguration;
}

void FakePolarisServer::acceptDescriptor(qintptr descriptor)
{
    auto state = std::make_unique<Connection>();
    auto* socket = new QSslSocket(this);
    state->socket = socket;
    if (!socket->setSocketDescriptor(descriptor)) {
        m_FailureLabel = QStringLiteral("accepted_socket_failed");
        delete socket;
        return;
    }
    socket->setSslConfiguration(m_ServerConfiguration);
    socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket->setReadBufferSize(MaximumSocketReadBufferBytes);
    m_Connections.push_back(std::move(state));

    connect(socket, &QSslSocket::encrypted, this, [this, socket] {
        Connection* state = connection(socket);
        if (state == nullptr) {
            return;
        }
        if (socket->peerCertificate() != m_ClientCertificate) {
            rejectConnection(socket, QStringLiteral("client_identity_rejected"));
            return;
        }
        state->authenticated = true;
        processInput(socket);
    });
    connect(socket, &QSslSocket::readyRead, this,
            [this, socket] { processInput(socket); });
    connect(socket, &QSslSocket::sslErrors, this,
            [this, socket](const QList<QSslError>&) {
        rejectConnection(socket, QStringLiteral("client_tls_rejected"));
    });
    connect(socket, &QSslSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
        Connection* state = connection(socket);
        if (state != nullptr && !state->authenticated &&
                m_FailureLabel.isEmpty()) {
            m_FailureLabel = QStringLiteral("client_tls_rejected");
        }
    });
    connect(socket, &QSslSocket::disconnected, this,
            [this, socket] { closeConnection(socket); });
    socket->startServerEncryption();
}

FakePolarisServer::Connection*
FakePolarisServer::connection(QSslSocket* socket)
{
    for (const auto& state : m_Connections) {
        if (state->socket == socket) {
            return state.get();
        }
    }
    return nullptr;
}

void FakePolarisServer::processInput(QSslSocket* socket)
{
    Connection* state = connection(socket);
    if (state == nullptr || !state->authenticated || state->handled) {
        return;
    }
    while (socket->bytesAvailable() > 0) {
        const qsizetype phaseLimit = state->headersParsed
            ? std::min(MaximumBodyBytes, state->contentLength)
            : MaximumHeaderBytes;
        const qsizetype detectionLimit = phaseLimit + 1;
        const qsizetype remaining = detectionLimit - state->input.size();
        if (remaining <= 0) {
            rejectConnection(
                socket, state->headersParsed
                    ? QStringLiteral("surplus_request_data")
                    : QStringLiteral("headers_too_large"));
            return;
        }
        const qint64 bytesToRead = std::min(
            socket->bytesAvailable(), qint64(remaining));
        QByteArray chunk = socket->read(bytesToRead);
        if (chunk.isEmpty()) {
            return;
        }
        state->input.append(chunk);

        if (!state->headersParsed) {
            const qsizetype headerEnd = state->input.indexOf("\r\n\r\n");
            const qsizetype checkedHeaderBytes = headerEnd >= 0
                ? headerEnd + 4 : state->input.size();
            if (hasInvalidHeaderLineEnding(
                    state->input.left(checkedHeaderBytes))) {
                rejectConnection(socket,
                                 QStringLiteral("invalid_line_ending"));
                return;
            }
            const qsizetype requestLineEnd = state->input.indexOf("\r\n");
            if (requestLineEnd > MaximumRequestLineBytes ||
                    (requestLineEnd < 0 &&
                     state->input.size() > MaximumRequestLineBytes)) {
                rejectConnection(
                    socket, QStringLiteral("request_line_too_large"));
                return;
            }
            if (headerEnd < 0) {
                if (state->input.size() > MaximumHeaderBytes) {
                    rejectConnection(socket,
                                     QStringLiteral("headers_too_large"));
                }
                continue;
            }
            if (headerEnd + 4 > MaximumHeaderBytes) {
                rejectConnection(socket, QStringLiteral("headers_too_large"));
                return;
            }
            const QByteArray headers = state->input.left(headerEnd);
            state->input.remove(0, headerEnd + 4);
            const QList<QByteArray> lines = headers.split('\n');
            if (lines.isEmpty()) {
                rejectConnection(socket,
                                 QStringLiteral("invalid_request_line"));
                return;
            }
            QByteArray requestLine = lines.constFirst();
            if (requestLine.endsWith('\r')) {
                requestLine.chop(1);
            }
            const QList<QByteArray> parts = requestLine.split(' ');
            if (parts.size() != 3 ||
                    parts.at(2) != QByteArrayLiteral("HTTP/1.1")) {
                rejectConnection(socket,
                                 QStringLiteral("invalid_request_line"));
                return;
            }
            state->method = parts.at(0).toUpper();
            state->path = normalizeTarget(parts.at(1));
            if ((state->method != QByteArrayLiteral("GET") &&
                 state->method != QByteArrayLiteral("POST")) ||
                    state->path.isEmpty()) {
                rejectConnection(socket,
                                 QStringLiteral("invalid_request_target"));
                return;
            }

            qsizetype contentLength = 0;
            bool sawLength = false;
            int hostCount = 0;
            for (qsizetype index = 1; index < lines.size(); ++index) {
                QByteArray line = lines.at(index);
                if (line.endsWith('\r')) {
                    line.chop(1);
                }
                const qsizetype colon = line.indexOf(':');
                if (colon <= 0) {
                    rejectConnection(socket,
                                     QStringLiteral("invalid_header"));
                    return;
                }
                const QByteArray originalName = line.left(colon);
                if (!isHttpToken(originalName)) {
                    rejectConnection(
                        socket, QStringLiteral("invalid_header_name"));
                    return;
                }
                const QByteArray name = originalName.toLower();
                const QByteArray value = line.mid(colon + 1).trimmed();
                if (name == QByteArrayLiteral("host")) {
                    ++hostCount;
                    if (hostCount != 1 || value.isEmpty()) {
                        rejectConnection(socket,
                                         QStringLiteral("invalid_host"));
                        return;
                    }
                }
                if (name == QByteArrayLiteral("transfer-encoding")) {
                    rejectConnection(
                        socket, QStringLiteral("unsupported_transfer"));
                    return;
                }
                if (name == QByteArrayLiteral("content-length")) {
                    bool ok = false;
                    const qlonglong parsed = value.toLongLong(&ok);
                    if (!ok || parsed < 0 ||
                            parsed > MaximumBodyBytes || sawLength) {
                        rejectConnection(
                            socket,
                            QStringLiteral("invalid_content_length"));
                        return;
                    }
                    contentLength = static_cast<qsizetype>(parsed);
                    sawLength = true;
                }
            }
            if (hostCount == 0) {
                rejectConnection(socket, QStringLiteral("missing_host"));
                return;
            }
            state->contentLength = contentLength;
            state->headersParsed = true;
        }

        if (state->input.size() > state->contentLength) {
            rejectConnection(socket, QStringLiteral("surplus_request_data"));
            return;
        }
        if (state->input.size() == state->contentLength) {
            state->body = std::move(state->input);
            state->input.clear();
            handleRequest(socket);
            return;
        }
    }
}

void FakePolarisServer::handleRequest(QSslSocket* socket)
{
    Connection* state = connection(socket);
    if (state == nullptr || state->handled) {
        return;
    }
    state->handled = true;
    m_History.append({state->method, state->path});
    if (m_Scripts.isEmpty()) {
        ++m_UnexpectedRequests;
        m_FailureLabel = QStringLiteral("script_exhausted");
        wipeBytes(state->body, &m_RequestBodyWipes,
                  &m_AllRequestBodyWipesWereZero);
        socket->disconnectFromHost();
        return;
    }

    ResponseScript script = m_Scripts.dequeue();
    if (script.method.toUpper() != state->method ||
            script.path != state->path) {
        ++m_UnexpectedRequests;
        m_FailureLabel = QStringLiteral("unexpected_method_or_path");
        wipeBytes(state->body, &m_RequestBodyWipes,
                  &m_AllRequestBodyWipesWereZero);
        socket->disconnectFromHost();
        return;
    }
    bool bodyAccepted = true;
    if (script.inspectBody) {
        try {
            bodyAccepted = script.inspectBody(state->body);
        }
        catch (...) {
            bodyAccepted = false;
        }
    }
    wipeBytes(state->body, &m_RequestBodyWipes,
              &m_AllRequestBodyWipesWereZero);
    if (!bodyAccepted) {
        ++m_UnexpectedRequests;
        m_FailureLabel = QStringLiteral("request_body_rejected");
        socket->disconnectFromHost();
        return;
    }
    writeResponse(socket, std::move(script));
}

void FakePolarisServer::writeResponse(
    QSslSocket* socket, ResponseScript script)
{
    auto state = std::make_shared<ResponseWriteState>(
        m_ResponseWipeAudit, socket, std::move(script));
    if (state->script.stall) {
        return;
    }
    if (state->script.disconnectBeforeHeaders) {
        socket->disconnectFromHost();
        return;
    }

    const int delay = std::max(0, state->script.delayBeforeHeadersMs);
    QTimer::singleShot(delay, socket,
        [this, state] { beginResponse(state); });
}

void FakePolarisServer::beginResponse(
    const std::shared_ptr<ResponseWriteState>& state)
{
    QSslSocket* socket = state->socket.data();
    if (socket == nullptr ||
            socket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    ResponseScript& script = state->script;
    if (!script.malformedResponse.isEmpty()) {
        socket->write(script.malformedResponse);
        wipeBytes(script.malformedResponse, &state->audit->wipeCount,
                  &state->audit->allBytesWereZero,
                  &state->audit->logicalBytesWiped);
        socket->disconnectFromHost();
        return;
    }

    qint64 bodySize = 0;
    for (const QByteArray& chunk : std::as_const(script.bodyChunks)) {
        bodySize += chunk.size();
    }
    QByteArray headers = QByteArrayLiteral("HTTP/1.1 ") +
        QByteArray::number(script.status) + ' ' +
        reasonPhrase(script.status) + QByteArrayLiteral("\r\n");
    bool hasContentLength = false;
    for (auto iterator = script.headers.cbegin();
         iterator != script.headers.cend(); ++iterator) {
        if (iterator.key().compare(QByteArrayLiteral("content-length"),
                                   Qt::CaseInsensitive) == 0) {
            hasContentLength = true;
        }
        headers += iterator.key() + QByteArrayLiteral(": ") +
            iterator.value() + QByteArrayLiteral("\r\n");
    }
    if (!hasContentLength) {
        headers += QByteArrayLiteral("Content-Length: ") +
            QByteArray::number(bodySize) + QByteArrayLiteral("\r\n");
    }
    headers += QByteArrayLiteral("Connection: close\r\n\r\n");
    socket->write(headers);
    if (script.disconnectAfterHeaders) {
        socket->disconnectFromHost();
        return;
    }
    writeNextResponseChunk(state);
}

void FakePolarisServer::writeNextResponseChunk(
    const std::shared_ptr<ResponseWriteState>& state)
{
    QSslSocket* socket = state->socket.data();
    if (socket == nullptr ||
            socket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    if (state->script.bodyChunks.isEmpty()) {
        socket->disconnectFromHost();
        return;
    }
    QByteArray chunk = state->script.bodyChunks.takeFirst();
    socket->write(chunk);
    wipeBytes(chunk, &state->audit->wipeCount,
              &state->audit->allBytesWereZero,
              &state->audit->logicalBytesWiped);
    const int delay = std::max(0, state->script.delayBetweenChunksMs);
    QTimer::singleShot(delay, socket,
        [this, state] { writeNextResponseChunk(state); });
}

void FakePolarisServer::closeConnection(QSslSocket* socket)
{
    for (const auto& state : m_Connections) {
        if (state->socket == socket) {
            state->pendingDelete = true;
            socket->deleteLater();
            if (m_PendingDeleteObserver) {
                m_PendingDeleteObserver();
            }
            // Keep this closed socket in the synchronous teardown set until
            // DeferredDelete actually runs. stop() can therefore cancel all
            // socket-context timers before owner members are destroyed.
            return;
        }
    }
}

void FakePolarisServer::rejectConnection(
    QSslSocket* socket, const QString& label)
{
    Connection* state = connection(socket);
    if (state != nullptr && !state->authenticated) {
        ++m_RejectedTlsConnections;
    }
    m_FailureLabel = label;
    socket->abort();
}
