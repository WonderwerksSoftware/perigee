#include "polarisadapter.h"

#include "perigee/actions/gamestreamadapter.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <optional>
#include <utility>

namespace {

constexpr auto CapabilitiesRoute = "/polaris/v1/capabilities";
constexpr auto SessionStatusRoute = "/polaris/v1/session/status";
constexpr auto ClientSettingsRoute = "/polaris/v1/client-settings";

enum class DiscoveryPart {
    Capabilities,
    Session,
    Settings,
};

class ApiClientTransport final : public PolarisTransport
{
public:
    explicit ApiClientTransport(const NvComputer& computer)
        : m_Client(std::make_unique<PolarisApiClient>(computer))
    {
    }

    RequestId get(const QString& endpoint, bool expectJson,
                  Completion completion,
                  PolarisRequestOptions options) override
    {
        return m_Client->get(endpoint, expectJson, std::move(completion), options);
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

QString firstDiscoveryError(const PolarisCapabilities& capabilities)
{
    return capabilities.errorCode;
}

}

struct PolarisAdapter::SharedState
{
    struct Generation {
        quint64 id = 0;
        QHash<PolarisTransport::RequestId, DiscoveryPart> partByRequest;
        QSet<PolarisTransport::RequestId> completedRequests;
        std::optional<PolarisResponse> capabilities;
        std::optional<PolarisResponse> session;
        std::optional<PolarisResponse> settings;
        bool cancellationIssued = false;
        bool terminal = false;
    };

    mutable QMutex mutex;
    bool alive = true;
    bool started = false;
    quint64 nextGeneration = 0;
    QUrl origin;
    std::optional<Generation> active;
    PolarisDiscoverySnapshot published;
};

PolarisAdapter::PolarisAdapter(
    GameStreamAdapter& localAdapter,
    std::unique_ptr<PolarisTransport> transport)
    : m_LocalAdapter(localAdapter)
    , m_Transport(std::move(transport))
    , m_State(std::make_shared<SharedState>())
{
    if (m_Transport != nullptr) {
        m_State->origin = m_Transport->pairedOrigin();
    }
}

PolarisAdapter::PolarisAdapter(GameStreamAdapter& localAdapter,
                               const NvComputer& computer)
    : PolarisAdapter(localAdapter,
                     std::make_unique<ApiClientTransport>(computer))
{
}

PolarisAdapter::~PolarisAdapter()
{
    const QVector<PolarisTransport::RequestId> requests =
        outstandingRequestIds();
    {
        QMutexLocker locker(&m_State->mutex);
        m_State->alive = false;
        m_State->active.reset();
    }
    if (m_Transport != nullptr) {
        for (PolarisTransport::RequestId request : requests) {
            m_Transport->cancel(request);
        }
    }
}

QVector<ActionDescriptor> PolarisAdapter::descriptors()
{
    return GameStreamAdapter::descriptors();
}

bool PolarisAdapter::startDiscovery()
{
    return beginGeneration(true);
}

bool PolarisAdapter::refresh()
{
    return beginGeneration(false);
}

bool PolarisAdapter::beginGeneration(bool initialOnly)
{
    if (m_Transport == nullptr) {
        return false;
    }

    QVector<PolarisTransport::RequestId> previousRequests;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_State->mutex);
        if (!m_State->alive || (initialOnly && m_State->started)) {
            return false;
        }
        m_State->started = true;
        if (m_State->active.has_value()) {
            for (auto it = m_State->active->partByRequest.cbegin();
                 it != m_State->active->partByRequest.cend(); ++it) {
                if (!m_State->active->completedRequests.contains(it.key())) {
                    previousRequests.push_back(it.key());
                }
            }
        }
        generation = ++m_State->nextGeneration;
        SharedState::Generation next;
        next.id = generation;
        m_State->active = std::move(next);
    }

    for (PolarisTransport::RequestId request : previousRequests) {
        m_Transport->cancel(request);
    }

    const auto submit = [this, generation](const QString& endpoint,
                                           DiscoveryPart part) {
        const std::weak_ptr<SharedState> weakState = m_State;
        const PolarisTransport::RequestId request = m_Transport->get(
            endpoint, true,
            [weakState, generation, part](PolarisTransport::RequestId requestId,
                                          const PolarisResponse& response) {
                const std::shared_ptr<SharedState> state = weakState.lock();
                if (!state) {
                    return;
                }
                QMutexLocker locker(&state->mutex);
                if (!state->alive || !state->active.has_value() ||
                        state->active->id != generation ||
                        state->active->terminal ||
                        state->active->completedRequests.contains(requestId) ||
                        !state->active->partByRequest.contains(requestId) ||
                        state->active->partByRequest.value(requestId) != part) {
                    return;
                }
                state->active->completedRequests.insert(requestId);
                switch (part) {
                case DiscoveryPart::Capabilities:
                    state->active->capabilities = response;
                    break;
                case DiscoveryPart::Session:
                    state->active->session = response;
                    break;
                case DiscoveryPart::Settings:
                    state->active->settings = response;
                    break;
                }

                if (state->active->capabilities.has_value()) {
                    const PolarisCapabilities capabilities =
                        PolarisModels::parseCapabilities(
                            *state->active->capabilities, state->origin);
                    if (capabilities.standardHost) {
                        PolarisDiscoverySnapshot fallback;
                        fallback.generation = generation;
                        fallback.complete = true;
                        fallback.standardHost = true;
                        fallback.capabilities = capabilities;
                        state->published = std::move(fallback);
                        state->active->terminal = true;
                        state->active->capabilities.reset();
                        state->active->session.reset();
                        state->active->settings.reset();
                        return;
                    }
                }

                if (!state->active->capabilities.has_value() ||
                        !state->active->session.has_value() ||
                        !state->active->settings.has_value()) {
                    return;
                }

                PolarisDiscoverySnapshot completed;
                completed.generation = generation;
                completed.complete = true;
                completed.capabilities = PolarisModels::parseCapabilities(
                    *state->active->capabilities, state->origin);
                completed.session = PolarisModels::parseSessionStatus(
                    *state->active->session, state->origin);
                completed.settings = PolarisModels::parseClientSettings(
                    *state->active->settings);
                completed.standardHost = completed.capabilities.standardHost;
                completed.errorCode = firstDiscoveryError(
                    completed.capabilities);
                state->published = std::move(completed);
                state->active->terminal = true;
                state->active->capabilities.reset();
                state->active->session.reset();
                state->active->settings.reset();
            });
        QMutexLocker locker(&m_State->mutex);
        if (m_State->alive && m_State->active.has_value() &&
                m_State->active->id == generation) {
            m_State->active->partByRequest.insert(request, part);
        }
        else {
            locker.unlock();
            m_Transport->cancel(request);
        }
    };

    submit(QString::fromLatin1(CapabilitiesRoute), DiscoveryPart::Capabilities);
    submit(QString::fromLatin1(SessionStatusRoute), DiscoveryPart::Session);
    submit(QString::fromLatin1(ClientSettingsRoute), DiscoveryPart::Settings);
    return true;
}

int PolarisAdapter::pumpCompletions(int maximum)
{
    if (m_Transport == nullptr || maximum <= 0) {
        return 0;
    }
    const int delivered = m_Transport->drainCompletions(
        std::min(maximum, CompletionPumpLimit));

    QVector<PolarisTransport::RequestId> cancellations;
    {
        QMutexLocker locker(&m_State->mutex);
        if (m_State->active.has_value() &&
                m_State->published.complete &&
                m_State->published.generation == m_State->active->id &&
                (m_State->published.standardHost ||
                 !m_State->published.errorCode.isEmpty()) &&
                !m_State->active->cancellationIssued) {
            for (auto it = m_State->active->partByRequest.cbegin();
                 it != m_State->active->partByRequest.cend(); ++it) {
                if (!m_State->active->completedRequests.contains(it.key())) {
                    cancellations.push_back(it.key());
                }
            }
            m_State->active->cancellationIssued = true;
        }
    }
    for (PolarisTransport::RequestId request : cancellations) {
        m_Transport->cancel(request);
    }
    return delivered;
}

bool PolarisAdapter::hasPendingDiscovery() const
{
    QMutexLocker locker(&m_State->mutex);
    return m_State->active.has_value() &&
        (!m_State->published.complete ||
         m_State->published.generation != m_State->active->id);
}

PolarisDiscoverySnapshot PolarisAdapter::discoverySnapshot() const
{
    QMutexLocker locker(&m_State->mutex);
    return m_State->published;
}

PolarisAvailability PolarisAdapter::availability(
    PolarisOperation operation) const
{
    return PolarisModels::availability(discoverySnapshot(), operation);
}

HostSnapshot PolarisAdapter::snapshot()
{
    HostSnapshot result = m_LocalAdapter.snapshot();
    const PolarisDiscoverySnapshot discovery = discoverySnapshot();
    result.advertisedCapabilities.unite(discovery.capabilities.features);
    result.actionStates.insert(
        QStringLiteral("clipboard.fetch-remote"),
        actionState(PolarisModels::availability(
            discovery, PolarisOperation::ClipboardRead)));
    result.actionStates.insert(
        QStringLiteral("clipboard.send-local"),
        actionState(PolarisModels::availability(
            discovery, PolarisOperation::ClipboardWrite)));
    result.actionStates.insert(
        QStringLiteral("host.command"),
        actionState(PolarisModels::availability(
            discovery, PolarisOperation::NamedCommand)));
    result.actionStates.insert(
        QStringLiteral("session.end-host"),
        actionState(PolarisModels::availability(
            discovery, PolarisOperation::StopSession)));
    result.actionStates.insert(
        QStringLiteral("display.switch"),
        actionState(PolarisModels::availability(
            discovery, PolarisOperation::DisplaySwitch)));
    return result;
}

ActionState PolarisAdapter::actionState(
    const PolarisAvailability& availability)
{
    ActionState state;
    state.enabled = availability.enabled;
    state.disabledCode = availability.errorCode;
    state.disabledReason = availability.reason;
    return state;
}

void PolarisAdapter::execute(const QString& actionId,
                             QVariantMap parameters,
                             Completion completion)
{
    const QVector<ActionDescriptor> localDescriptors =
        GameStreamAdapter::descriptors();
    const bool local = std::any_of(
        localDescriptors.cbegin(), localDescriptors.cend(),
        [&actionId](const ActionDescriptor& descriptor) {
            return descriptor.id == actionId;
        });
    if (local) {
        m_LocalAdapter.execute(actionId, std::move(parameters),
                               std::move(completion));
        return;
    }
    if (completion) {
        completion({false, {}, QStringLiteral("action_unavailable"),
                    QStringLiteral("Action is unavailable.")});
    }
}

void PolarisAdapter::cancel(const QString& resourceKey)
{
    m_LocalAdapter.cancel(resourceKey);
}

QVector<PolarisTransport::RequestId> PolarisAdapter::outstandingRequestIds() const
{
    QVector<PolarisTransport::RequestId> result;
    QMutexLocker locker(&m_State->mutex);
    if (!m_State->active.has_value()) {
        return result;
    }
    for (auto it = m_State->active->partByRequest.cbegin();
         it != m_State->active->partByRequest.cend(); ++it) {
        if (!m_State->active->completedRequests.contains(it.key())) {
            result.push_back(it.key());
        }
    }
    return result;
}
