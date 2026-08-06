#pragma once

#include "perigee/actions/hostadapter.h"
#include "polarisapiclient.h"
#include "polarismodels.h"

#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

class GameStreamAdapter;
class NvComputer;

class PolarisClipboard
{
public:
    virtual ~PolarisClipboard() = default;
    virtual bool readText(QByteArray* text) = 0;
    virtual bool writeText(const QByteArray& text) = 0;
    virtual void sensitiveBufferWiped(const QByteArray&) {}
};

class PolarisTransport
{
public:
    using RequestId = PolarisApiClient::RequestId;
    using Completion = PolarisApiClient::Completion;

    virtual ~PolarisTransport() = default;
    virtual RequestId get(const QString& endpoint, bool expectJson,
                          Completion completion,
                          PolarisRequestOptions options = {}) = 0;
    virtual RequestId post(const QString& endpoint, const QByteArray& body,
                           bool expectJson, Completion completion,
                           PolarisRequestOptions options = {}) = 0;
    virtual RequestId fetchClipboard(
        std::optional<qint64> advertisedLimit, Completion completion,
        PolarisRequestOptions options = {}) = 0;
    virtual RequestId sendClipboard(
        const QByteArray& body, Completion completion,
        PolarisRequestOptions options = {}) = 0;
    virtual bool cancel(RequestId requestId) = 0;
    virtual int drainCompletions(int maximum) = 0;
    virtual qsizetype pendingCompletionCount() const = 0;
    virtual QUrl pairedOrigin() const = 0;
};

class PolarisAdapter final : public HostAdapter
{
public:
    static constexpr int CompletionPumpLimit = 8;

    PolarisAdapter(GameStreamAdapter& localAdapter,
                   std::unique_ptr<PolarisTransport> transport,
                   std::unique_ptr<PolarisClipboard> clipboard = {});
    PolarisAdapter(GameStreamAdapter& localAdapter,
                   const NvComputer& computer);
    ~PolarisAdapter() override;

    PolarisAdapter(const PolarisAdapter&) = delete;
    PolarisAdapter& operator=(const PolarisAdapter&) = delete;

    static QVector<ActionDescriptor> descriptors();

    bool startDiscovery();
    bool refresh();
    int pumpCompletions(int maximum = CompletionPumpLimit);
    bool hasPendingDiscovery() const;
    PolarisDiscoverySnapshot discoverySnapshot() const;
    PolarisAvailability availability(PolarisOperation operation) const;

    HostSnapshot snapshot() override;
    void cancel(const QString& resourceKey) override;

private:
    enum class DiscoveryPart {
        Capabilities,
        Session,
        Settings,
        Commands,
    };

    void execute(const QString& actionId,
                 QVariantMap parameters,
                 Completion completion) override;

    struct SharedState;
    struct ActionRequest;

    static bool beginGeneration(
        const std::shared_ptr<SharedState>& state,
        const std::shared_ptr<PolarisTransport>& transport,
        bool initialOnly);
    static void submitRequest(
        const std::shared_ptr<SharedState>& state,
        const std::shared_ptr<PolarisTransport>& transport,
        quint64 generation, const QString& endpoint, DiscoveryPart part);
    static void handleDiscoveryCompletion(
        const std::weak_ptr<SharedState>& weakState,
        quint64 generation, DiscoveryPart part,
        PolarisTransport::RequestId requestId,
        const PolarisResponse& response);
    enum class ActionKind {
        Command,
        StopSession,
        ClipboardSend,
        ClipboardFetch,
        QualityMode,
        Bitrate,
    };
    void submitAction(const QString& actionId, const QString& resourceKey,
                      const QString& endpoint, QByteArray body,
                      std::optional<qint64> clipboardLimit,
                      ActionKind kind, ActionState authoritativeState,
                      Completion completion);
    static void handleActionCompletion(
        const std::weak_ptr<SharedState>& weakState,
        const std::shared_ptr<ActionRequest>& request,
        PolarisTransport::RequestId requestId,
        const PolarisResponse& response);
    static ActionState actionState(const PolarisAvailability& availability);

    GameStreamAdapter& m_LocalAdapter;
    std::shared_ptr<PolarisTransport> m_Transport;
    std::shared_ptr<PolarisClipboard> m_Clipboard;
    std::shared_ptr<SharedState> m_State;
    bool m_DiscoveryEnabled = true;
};
