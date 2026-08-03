#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QSslSocket;

class FakePolarisServer final : public QObject
{
public:
    struct RequestRecord
    {
        QByteArray method;
        QString path;
    };

    struct ResponseScript
    {
        QByteArray method = QByteArrayLiteral("GET");
        QString path;
        int status = 200;
        QHash<QByteArray, QByteArray> headers;
        QList<QByteArray> bodyChunks;
        int delayBeforeHeadersMs = 0;
        int delayBetweenChunksMs = 0;
        bool stall = false;
        bool disconnectBeforeHeaders = false;
        bool disconnectAfterHeaders = false;
        QByteArray malformedResponse;
        std::function<bool(QByteArray&)> inspectBody;
    };

    struct ResponseWipeAudit
    {
        int wipeCount = 0;
        qsizetype logicalBytesWiped = 0;
        bool allBytesWereZero = true;
    };

    explicit FakePolarisServer(QObject* parent = nullptr);
    ~FakePolarisServer() override;

    bool start();
    bool stop();
    bool isListening() const;
    QHostAddress address() const;
    quint16 port() const;
    QString failureLabel() const;

    void enqueue(ResponseScript script);
    QList<RequestRecord> requestHistory() const;
    int unexpectedRequestCount() const;
    int rejectedTlsConnectionCount() const;
    int requestBodyWipeCount() const;
    bool allRequestBodyWipesWereZero() const;
    int responseBodyWipeCount() const;
    bool allResponseBodyWipesWereZero() const;
    std::shared_ptr<const ResponseWipeAudit> responseWipeAudit() const;
    qsizetype pendingDeleteConnectionCount() const;
    qsizetype synchronousTeardownSocketCount() const;
    void setPendingDeleteObserver(std::function<void()> observer);
    qsizetype pendingScriptCount() const;
    bool waitForRequestCount(qsizetype count, int timeoutMs = 1000);

    QSslCertificate serverCertificate() const;
    QSslCertificate clientCertificate() const;
    QSslConfiguration clientSslConfiguration() const;
    QSslConfiguration emptyClientSslConfiguration() const;
    QSslConfiguration wrongClientSslConfiguration() const;

private:
    class Listener;
    struct Connection;
    struct ResponseWriteState;

    bool loadFixtures();
    void acceptDescriptor(qintptr descriptor);
    void processInput(QSslSocket* socket);
    void handleRequest(QSslSocket* socket);
    void writeResponse(QSslSocket* socket, ResponseScript script);
    void beginResponse(const std::shared_ptr<ResponseWriteState>& state);
    void writeNextResponseChunk(
        const std::shared_ptr<ResponseWriteState>& state);
    void closeConnection(QSslSocket* socket);
    void rejectConnection(QSslSocket* socket, const QString& label);
    Connection* connection(QSslSocket* socket);

    std::unique_ptr<Listener> m_Listener;
    std::vector<std::unique_ptr<Connection>> m_Connections;
    QQueue<ResponseScript> m_Scripts;
    QList<RequestRecord> m_History;
    QSslCertificate m_CaCertificate;
    QSslCertificate m_ServerCertificate;
    QSslCertificate m_ClientCertificate;
    QSslConfiguration m_ServerConfiguration;
    QSslConfiguration m_ClientConfiguration;
    QSslConfiguration m_WrongClientConfiguration;
    QString m_FailureLabel;
    int m_UnexpectedRequests = 0;
    int m_RejectedTlsConnections = 0;
    int m_RequestBodyWipes = 0;
    bool m_AllRequestBodyWipesWereZero = true;
    std::shared_ptr<ResponseWipeAudit> m_ResponseWipeAudit;
    std::function<void()> m_PendingDeleteObserver;
};
