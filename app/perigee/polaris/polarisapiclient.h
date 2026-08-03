#pragma once

#include "backend/nvaddress.h"
#include "polarisresponse.h"

#include <QByteArray>
#include <QList>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

class NvComputer;
class QObject;
class QNetworkAccessManager;
class QNetworkReply;

struct PolarisRequestOptions
{
    int totalTimeoutMs = 5000;
    int idleTimeoutMs = 2000;
    qint64 maxBodyBytes = 1024 * 1024;
};

class PolarisNetworkBackend
{
public:
    virtual ~PolarisNetworkBackend() = default;

    virtual QNetworkAccessManager* createNetworkAccessManager(
        QObject* parent) = 0;
    virtual QSslCertificate peerLeafCertificate(
        const QNetworkReply* reply) const = 0;
};

class PolarisApiClient final
{
public:
    using RequestId = quint64;
    using Completion =
        std::function<void(RequestId, const PolarisResponse&)>;

    explicit PolarisApiClient(
        const NvComputer& computer,
        std::unique_ptr<PolarisNetworkBackend> backend = {});
    ~PolarisApiClient();

    PolarisApiClient(const PolarisApiClient&) = delete;
    PolarisApiClient& operator=(const PolarisApiClient&) = delete;

    RequestId get(const QString& endpoint, bool expectJson,
                  Completion completion,
                  PolarisRequestOptions options = {});
    RequestId request(const QByteArray& method, const QString& endpoint,
                      const QByteArray& body, bool expectJson,
                      Completion completion,
                      PolarisRequestOptions options = {});
    RequestId fetchClipboard(std::optional<qint64> advertisedLimit,
                             Completion completion,
                             PolarisRequestOptions options = {});

    bool cancel(RequestId requestId);
    int drainCompletions(int maximum = 16);
    qsizetype pendingCompletionCount() const;

    static QUrl pairedOrigin(const NvAddress& address, quint16 httpsPort);
    static QUrl resolveAdvertisedEndpoint(const QUrl& pairedOrigin,
                                          const QString& endpoint,
                                          QString* errorCode = nullptr);
    static QNetworkRequest makeRequest(
        const QUrl& url, const QSslConfiguration& identity);
    static bool acceptsExactPairedIdentity(
        const QSslCertificate& expectedLeaf,
        const QSslCertificate& peerLeaf,
        const QList<QSslError>& errors = {});

private:
    struct Private;
    std::unique_ptr<Private> d;
};
