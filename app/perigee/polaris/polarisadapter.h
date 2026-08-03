#pragma once

#include "perigee/actions/hostadapter.h"
#include "polarisapiclient.h"
#include "polarismodels.h"

#include <QUrl>
#include <QVector>

#include <memory>

class GameStreamAdapter;
class NvComputer;

class PolarisTransport
{
public:
    using RequestId = PolarisApiClient::RequestId;
    using Completion = PolarisApiClient::Completion;

    virtual ~PolarisTransport() = default;
    virtual RequestId get(const QString& endpoint, bool expectJson,
                          Completion completion,
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
                   std::unique_ptr<PolarisTransport> transport);
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

    bool beginGeneration(bool initialOnly);
    void submitRequest(quint64 generation, const QString& endpoint,
                       DiscoveryPart part);
    static void handleDiscoveryCompletion(
        const std::weak_ptr<SharedState>& weakState,
        quint64 generation, DiscoveryPart part,
        PolarisTransport::RequestId requestId,
        const PolarisResponse& response);
    QVector<PolarisTransport::RequestId> outstandingRequestIds() const;
    static ActionState actionState(const PolarisAvailability& availability);

    GameStreamAdapter& m_LocalAdapter;
    std::unique_ptr<PolarisTransport> m_Transport;
    std::shared_ptr<SharedState> m_State;
};
