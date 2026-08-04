#include "polarisadapter.h"

#include "perigee/actions/gamestreamadapter.h"
#include "perigee/display/sessiontransitioncoordinator.h"

#include <SDL.h>

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringDecoder>
#else
#include <QTextCodec>
#endif
#include <QThread>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace {

constexpr auto CapabilitiesRoute = "/polaris/v1/capabilities";
constexpr auto SessionStatusRoute = "/polaris/v1/session/status";
constexpr auto ClientSettingsRoute = "/polaris/v1/client-settings";
constexpr auto DisplayPrefix = "display.target.";

QString displayActionId(const DisplayTarget& target)
{
    const QByteArray key = target.stableKey().toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(DisplayPrefix) + QString::fromLatin1(key);
}

QVariantMap displayMetadata(const DisplayTarget& target)
{
    return {
        {QStringLiteral("kind"), target.kind},
        {QStringLiteral("id"), target.id},
        {QStringLiteral("label"), target.label},
        {QStringLiteral("available"), target.available},
        {QStringLiteral("current"), target.current},
        {QStringLiteral("requires_reconnect"), target.requiresReconnect},
        {QStringLiteral("unavailable_reason"), target.unavailableReason},
    };
}

ActionDescriptor polarisDescriptor(
    const char* id, const char* label, ActionCategory category,
    const char* resourceKey, ConfirmationPolicy confirmation,
    const char* confirmationMessage = "")
{
    return {
        QString::fromLatin1(id),
        QString::fromLatin1(label),
        category,
        {},
        QString::fromLatin1(resourceKey),
        {},
        0,
        confirmation,
        QString::fromLatin1(confirmationMessage),
    };
}

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

void wipeBytes(void* data, qsizetype size)
{
    volatile unsigned char* bytes =
        static_cast<volatile unsigned char*>(data);
    for (qsizetype index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
}

void wipeSensitive(QByteArray& bytes, PolarisClipboard* clipboard)
{
    const qsizetype capacity = bytes.capacity();
    if (capacity > bytes.size()) {
        bytes.resize(capacity);
    }
    if (!bytes.isEmpty()) {
        wipeBytes(bytes.data(), bytes.size());
    }
    if (clipboard != nullptr) {
        try {
            clipboard->sensitiveBufferWiped(bytes);
        }
        catch (...) {
            // Sensitive-data observers are diagnostic hooks and must not
            // prevent erasure or escape cleanup paths.
        }
    }
    bytes.clear();
    bytes.squeeze();
}

class ScopedSensitiveWipe final
{
public:
    ScopedSensitiveWipe(
        QByteArray& bytes, std::shared_ptr<PolarisClipboard> clipboard)
        : m_Bytes(bytes)
        , m_Clipboard(std::move(clipboard))
    {
    }

    ~ScopedSensitiveWipe()
    {
        wipeSensitive(m_Bytes, m_Clipboard.get());
    }

    ScopedSensitiveWipe(const ScopedSensitiveWipe&) = delete;
    ScopedSensitiveWipe& operator=(const ScopedSensitiveWipe&) = delete;

private:
    QByteArray& m_Bytes;
    std::shared_ptr<PolarisClipboard> m_Clipboard;
};

bool strictUtf8(const QByteArray& bytes)
{
    if (bytes.contains('\0')) {
        return false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QStringDecoder decoder(
        QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder.decode(bytes);
    Q_UNUSED(decoded);
    return !decoder.hasError();
#else
    QTextCodec* codec = QTextCodec::codecForName("UTF-8");
    if (codec == nullptr) {
        return false;
    }
    QTextCodec::ConverterState state;
    const QString decoded = codec->toUnicode(bytes.constData(), bytes.size(), &state);
    Q_UNUSED(decoded);
    return state.invalidChars == 0 && state.remainingChars == 0;
#endif
}

class SdlClipboard final : public PolarisClipboard
{
public:
    bool readText(QByteArray* text) override
    {
        if (text == nullptr) {
            return false;
        }
        char* sdlText = SDL_GetClipboardText();
        if (sdlText == nullptr) {
            return false;
        }
        const size_t length = std::strlen(sdlText);
        *text = QByteArray(sdlText, static_cast<qsizetype>(length));
        wipeBytes(sdlText, static_cast<qsizetype>(length + 1));
        SDL_free(sdlText);
        return true;
    }

    bool writeText(const QByteArray& text) override
    {
        return SDL_SetClipboardText(text.constData()) == 0;
    }
};

QString firstDiscoveryError(const PolarisCapabilities& capabilities)
{
    return capabilities.errorCode;
}

ActionResult actionFailure(const QString& errorCode,
                           const QString& userMessage)
{
    return {false, {}, errorCode, userMessage};
}

ActionResult unavailableAction(const PolarisAvailability& availability)
{
    return actionFailure(
        availability.errorCode.isEmpty()
            ? QStringLiteral("action_unavailable")
            : availability.errorCode,
        availability.reason.isEmpty()
            ? QStringLiteral("Action is unavailable.")
            : availability.reason);
}

QByteArray compactObject(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

}

struct PolarisAdapter::ActionRequest
{
    PolarisTransport::RequestId requestId = 0;
    PolarisTransport::RequestId earlyRequestId = 0;
    QString actionId;
    QString resourceKey;
    ActionKind kind = ActionKind::Command;
    ActionState authoritativeState;
    QByteArray sensitiveBytes;
    std::optional<qint64> clipboardLimit;
    Completion completion;
    std::optional<PolarisResponse> earlyResponse;
    bool registered = false;
    bool finished = false;
};

struct PolarisAdapter::SharedState
{
    struct Generation {
        quint64 id = 0;
        QHash<PolarisTransport::RequestId, DiscoveryPart> partByRequest;
        QSet<PolarisTransport::RequestId> completedRequests;
        std::optional<PolarisResponse> capabilities;
        std::optional<PolarisResponse> session;
        std::optional<PolarisResponse> settings;
        std::optional<PolarisResponse> commands;
        bool commandsRequired = false;
        bool commandRequestScheduled = false;
        bool cancellationIssued = false;
        bool terminal = false;
    };

    mutable QMutex mutex;
    bool alive = true;
    bool started = false;
    quint64 nextGeneration = 0;
    QUrl origin;
    Qt::HANDLE sdlThread = nullptr;
    std::shared_ptr<PolarisClipboard> clipboard;
    std::optional<Generation> active;
    QHash<PolarisTransport::RequestId, std::shared_ptr<ActionRequest>> actions;
    bool refreshRequested = false;
    PolarisDiscoverySnapshot published;
};

PolarisAdapter::PolarisAdapter(
    GameStreamAdapter& localAdapter,
    std::unique_ptr<PolarisTransport> transport,
    std::unique_ptr<PolarisClipboard> clipboard,
    SessionTransitionCoordinator* transitionCoordinator)
    : m_LocalAdapter(localAdapter)
    , m_Transport(std::move(transport))
    , m_Clipboard(clipboard != nullptr
          ? std::shared_ptr<PolarisClipboard>(std::move(clipboard))
          : std::make_shared<SdlClipboard>())
    , m_State(std::make_shared<SharedState>())
    , m_TransitionCoordinator(transitionCoordinator)
{
    m_State->sdlThread = QThread::currentThreadId();
    m_State->clipboard = m_Clipboard;
    if (m_Transport != nullptr) {
        m_State->origin = m_Transport->pairedOrigin();
    }
}

PolarisAdapter::PolarisAdapter(GameStreamAdapter& localAdapter,
                               const NvComputer& computer,
                               SessionTransitionCoordinator* transitionCoordinator)
    : PolarisAdapter(localAdapter,
                     std::make_unique<ApiClientTransport>(computer), {},
                     transitionCoordinator)
{
}

PolarisAdapter::~PolarisAdapter()
{
    const std::shared_ptr<SharedState> state = m_State;
    const std::shared_ptr<PolarisTransport> transport = m_Transport;
    const std::shared_ptr<PolarisClipboard> clipboard = m_Clipboard;
    QVector<PolarisTransport::RequestId> requests;
    QVector<QByteArray> sensitiveBuffers;
    {
        QMutexLocker locker(&state->mutex);
        state->alive = false;
        if (state->active.has_value()) {
            for (auto it = state->active->partByRequest.cbegin();
                 it != state->active->partByRequest.cend(); ++it) {
                if (!state->active->completedRequests.contains(it.key())) {
                    requests.push_back(it.key());
                }
            }
        }
        state->active.reset();
        for (auto it = state->actions.begin();
             it != state->actions.end(); ++it) {
            const std::shared_ptr<ActionRequest>& request = it.value();
            if (!request->finished) {
                requests.push_back(it.key());
            }
            sensitiveBuffers.push_back(
                std::move(request->sensitiveBytes));
            if (request->earlyResponse.has_value()) {
                sensitiveBuffers.push_back(
                    std::move(request->earlyResponse->body));
            }
            request->finished = true;
            request->completion = {};
            request->earlyResponse.reset();
        }
        state->actions.clear();
    }
    for (QByteArray& bytes : sensitiveBuffers) {
        wipeSensitive(bytes, clipboard.get());
    }
    if (transport != nullptr) {
        for (PolarisTransport::RequestId request : requests) {
            transport->cancel(request);
        }
    }
}

QVector<ActionDescriptor> PolarisAdapter::descriptors()
{
    QVector<ActionDescriptor> result = GameStreamAdapter::descriptors();
    result.push_back(polarisDescriptor(
        "display.switch", "Select display", ActionCategory::Display,
        "display.selection", ConfirmationPolicy::Never));
    result.push_back(polarisDescriptor(
        "clipboard.send-local", "Send local clipboard to host",
        ActionCategory::Clipboard, "clipboard",
        ConfirmationPolicy::Never));
    result.push_back(polarisDescriptor(
        "clipboard.fetch-remote", "Copy host clipboard to local",
        ActionCategory::Clipboard, "clipboard",
        ConfirmationPolicy::Never));
    result.push_back(polarisDescriptor(
        "session.end-host", "End host session",
        ActionCategory::Session, "session.lifecycle",
        ConfirmationPolicy::Always,
        "End the host session for all connected clients? This is different "
        "from disconnecting this client."));
    result.push_back(polarisDescriptor(
        "host.command", "Host command", ActionCategory::Session,
        "host.command", ConfirmationPolicy::WhenDisruptive));
    return result;
}

bool PolarisAdapter::startDiscovery()
{
    return beginGeneration(m_State, m_Transport, true);
}

bool PolarisAdapter::refresh()
{
    return beginGeneration(m_State, m_Transport, false);
}

bool PolarisAdapter::beginGeneration(
    const std::shared_ptr<SharedState>& state,
    const std::shared_ptr<PolarisTransport>& transport,
    bool initialOnly)
{
    if (transport == nullptr) {
        return false;
    }

    QVector<PolarisTransport::RequestId> previousRequests;
    quint64 generation = 0;
    {
        QMutexLocker locker(&state->mutex);
        if (!state->alive || (initialOnly && state->started)) {
            return false;
        }
        state->started = true;
        if (state->active.has_value()) {
            for (auto it = state->active->partByRequest.cbegin();
                 it != state->active->partByRequest.cend(); ++it) {
                if (!state->active->completedRequests.contains(it.key())) {
                    previousRequests.push_back(it.key());
                }
            }
        }
        generation = ++state->nextGeneration;
        SharedState::Generation next;
        next.id = generation;
        state->active = std::move(next);
    }

    for (PolarisTransport::RequestId request : previousRequests) {
        transport->cancel(request);
    }

    submitRequest(state, transport, generation,
                  QString::fromLatin1(CapabilitiesRoute),
                  DiscoveryPart::Capabilities);
    submitRequest(state, transport, generation,
                  QString::fromLatin1(SessionStatusRoute),
                  DiscoveryPart::Session);
    submitRequest(state, transport, generation,
                  QString::fromLatin1(ClientSettingsRoute),
                  DiscoveryPart::Settings);
    return true;
}

void PolarisAdapter::submitRequest(
    const std::shared_ptr<SharedState>& state,
    const std::shared_ptr<PolarisTransport>& transport,
    quint64 generation, const QString& endpoint, DiscoveryPart part)
{
    const std::weak_ptr<SharedState> weakState = state;
    const PolarisTransport::RequestId request = transport->get(
        endpoint, true,
        [weakState, generation, part](PolarisTransport::RequestId requestId,
                                      const PolarisResponse& response) {
            handleDiscoveryCompletion(weakState, generation, part,
                                      requestId, response);
        });
    QMutexLocker locker(&state->mutex);
    if (state->alive && state->active.has_value() &&
            state->active->id == generation &&
            !state->active->terminal) {
        state->active->partByRequest.insert(request, part);
    }
    else {
        locker.unlock();
        transport->cancel(request);
    }
}

void PolarisAdapter::handleDiscoveryCompletion(
    const std::weak_ptr<SharedState>& weakState,
    quint64 generation, DiscoveryPart part,
    PolarisTransport::RequestId requestId,
    const PolarisResponse& response)
{
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
    case DiscoveryPart::Commands:
        state->active->commands = response;
        break;
    }

    PolarisCapabilities capabilities;
    if (state->active->capabilities.has_value()) {
        capabilities = PolarisModels::parseCapabilities(
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
            state->active->commands.reset();
            return;
        }
        state->active->commandsRequired = capabilities.valid &&
            capabilities.features.contains(
                QStringLiteral("named_commands_v1")) &&
            capabilities.commandsEndpoint.usable;
    }

    if (!state->active->capabilities.has_value() ||
            !state->active->session.has_value() ||
            !state->active->settings.has_value() ||
            (state->active->commandsRequired &&
             !state->active->commands.has_value())) {
        return;
    }

    PolarisDiscoverySnapshot completed;
    completed.generation = generation;
    completed.complete = true;
    completed.capabilities = std::move(capabilities);
    if (state->active->commandsRequired) {
        const PolarisCommandCatalog catalog = PolarisModels::parseCommands(
            *state->active->commands,
            completed.capabilities.commandsEndpoint);
        completed.capabilities.commandCatalogValid = catalog.valid;
        completed.capabilities.commandCatalogErrorCode = catalog.errorCode;
        completed.capabilities.commands = catalog.commands;
    }
    completed.session = PolarisModels::parseSessionStatus(
        *state->active->session, state->origin);
    completed.settings = PolarisModels::parseClientSettings(
        *state->active->settings);
    completed.standardHost = completed.capabilities.standardHost;
    completed.errorCode = firstDiscoveryError(completed.capabilities);
    state->published = std::move(completed);
    state->active->terminal = true;
    state->active->capabilities.reset();
    state->active->session.reset();
    state->active->settings.reset();
    state->active->commands.reset();
}

int PolarisAdapter::pumpCompletions(int maximum)
{
    const std::shared_ptr<SharedState> state = m_State;
    const std::shared_ptr<PolarisTransport> transport = m_Transport;
    if (transport == nullptr || maximum <= 0) {
        return 0;
    }
    const int delivered = transport->drainCompletions(
        std::min(maximum, CompletionPumpLimit));

    quint64 commandGeneration = 0;
    QString commandEndpoint;
    {
        QMutexLocker locker(&state->mutex);
        if (!state->alive) {
            return delivered;
        }
        if (state->active.has_value() &&
                !state->active->terminal &&
                state->active->commandsRequired &&
                !state->active->commandRequestScheduled &&
                state->active->capabilities.has_value()) {
            const PolarisCapabilities capabilities =
                PolarisModels::parseCapabilities(
                    *state->active->capabilities, state->origin);
            if (capabilities.commandsEndpoint.usable) {
                state->active->commandRequestScheduled = true;
                commandGeneration = state->active->id;
                commandEndpoint = capabilities.commandsEndpoint.advertised;
            }
        }
    }
    if (!commandEndpoint.isEmpty()) {
        submitRequest(state, transport, commandGeneration,
                      commandEndpoint, DiscoveryPart::Commands);
    }

    QVector<PolarisTransport::RequestId> cancellations;
    {
        QMutexLocker locker(&state->mutex);
        if (!state->alive) {
            return delivered;
        }
        if (state->active.has_value() &&
                state->published.complete &&
                state->published.generation == state->active->id &&
                (state->published.standardHost ||
                 !state->published.errorCode.isEmpty()) &&
                !state->active->cancellationIssued) {
            for (auto it = state->active->partByRequest.cbegin();
                 it != state->active->partByRequest.cend(); ++it) {
                if (!state->active->completedRequests.contains(it.key())) {
                    cancellations.push_back(it.key());
                }
            }
            state->active->cancellationIssued = true;
        }
    }
    for (PolarisTransport::RequestId request : cancellations) {
        transport->cancel(request);
    }
    bool refreshRequested = false;
    {
        QMutexLocker locker(&state->mutex);
        if (!state->alive) {
            return delivered;
        }
        if (state->refreshRequested) {
            state->refreshRequested = false;
            refreshRequested = true;
        }
    }
    if (refreshRequested) {
        beginGeneration(state, transport, false);
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

void PolarisAdapter::postDisplayTarget(
    const DisplayTarget& target, const QString& sessionToken,
    quint64 transactionEpoch, DisplayPostCompletion completion)
{
    postDisplayTarget(target, sessionToken, transactionEpoch,
                      discoverySnapshot(), std::move(completion));
}

void PolarisAdapter::postDisplayTarget(
    const DisplayTarget& target, const QString& sessionToken,
    quint64 transactionEpoch,
    const PolarisDiscoverySnapshot& authorizationSnapshot,
    DisplayPostCompletion completion)
{
    const PolarisDiscoverySnapshot currentDiscovery = discoverySnapshot();
    const PolarisDiscoverySnapshot& discovery = currentDiscovery.complete
        ? currentDiscovery : authorizationSnapshot;
    const PolarisAvailability available = PolarisModels::availability(
        discovery, PolarisOperation::DisplaySwitch);
    const auto advertisedTarget = std::find_if(
        discovery.settings.targets.cbegin(), discovery.settings.targets.cend(),
        [&target](const DisplayTarget& candidate) {
            return candidate.stableKey() == target.stableKey();
        });
    const auto reject = [&](const QString& code, const QString& message) {
        if (completion) {
            completion(transactionEpoch, false, code, message);
        }
    };
    if (!available.enabled) {
        reject(available.errorCode.isEmpty()
                   ? QStringLiteral("action_unavailable")
                   : available.errorCode,
               available.reason.isEmpty()
                   ? QStringLiteral("Display switching is unavailable.")
                   : available.reason);
        return;
    }
    if (transactionEpoch == 0 || sessionToken.isEmpty() ||
            !discovery.session.tokenValid ||
            sessionToken != discovery.session.sessionToken) {
        reject(QStringLiteral("stale_session"),
               QStringLiteral("The host session changed."));
        return;
    }
    if (advertisedTarget == discovery.settings.targets.cend() ||
            !advertisedTarget->available ||
            !discovery.capabilities.clientSettingsEndpoint.usable) {
        reject(QStringLiteral("stale_catalog"),
               QStringLiteral("The display target catalog changed."));
        return;
    }
    const QByteArray body = DisplayTransaction::postBody(target, sessionToken);
    if (body.isEmpty()) {
        reject(QStringLiteral("invalid_request"),
               QStringLiteral("Polaris rejected the display target."));
        return;
    }

    ActionState authoritativeState = actionState(available);
    authoritativeState.value = displayMetadata(*advertisedTarget);
    submitAction(
        displayActionId(*advertisedTarget), QStringLiteral("display.selection"),
        discovery.capabilities.clientSettingsEndpoint.advertised, body,
        std::nullopt, ActionKind::DisplayTarget,
        std::move(authoritativeState),
        [transactionEpoch, completion = std::move(completion)](
            const ActionResult& result) {
            if (completion) {
                completion(transactionEpoch, result.ok, result.errorCode,
                           result.userMessage);
            }
        });
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
    const PolarisAvailability commandAvailability =
        PolarisModels::availability(discovery, PolarisOperation::NamedCommand);
    ActionState commandTemplate = actionState(commandAvailability);
    commandTemplate.visible = false;
    result.actionStates.insert(QStringLiteral("host.command"), commandTemplate);
    for (const NamedCommandMetadata& command :
         discovery.capabilities.commands) {
        if (command.index < 0) {
            continue;
        }
        ActionState commandState = actionState(commandAvailability);
        commandState.disruptive = command.risk != QStringLiteral("safe");
        commandState.value = QVariantMap{
            {QStringLiteral("index"), command.index},
            {QStringLiteral("name"), command.displayName},
            {QStringLiteral("risk"), command.risk},
        };
        result.actionStates.insert(
            QStringLiteral("host.command.%1").arg(command.index),
            std::move(commandState));
    }
    result.actionStates.insert(
        QStringLiteral("session.end-host"),
        actionState(PolarisModels::availability(
            discovery, PolarisOperation::StopSession)));
    ActionState displayTemplate = actionState(PolarisModels::availability(
        discovery, PolarisOperation::DisplaySwitch));
    if (m_TransitionCoordinator != nullptr) {
        displayTemplate =
            m_TransitionCoordinator->decorateDisplaySelectionState(
                std::move(displayTemplate));
    }
    if (discovery.standardHost) {
        displayTemplate.visible = false;
    }
    else if (!discovery.settings.targets.isEmpty()) {
        displayTemplate.visible = false;
        for (const DisplayTarget& target : discovery.settings.targets) {
            ActionState targetState = actionState(
                PolarisModels::availability(
                    discovery, PolarisOperation::DisplaySwitch));
            targetState.value = displayMetadata(target);
            targetState.invocationState =
                DisplayTransaction::catalogIdentity(discovery);
            if (!target.available) {
                targetState.enabled = false;
                targetState.disabledCode = QStringLiteral("target_unavailable");
                targetState.disabledReason = target.unavailableReason.isEmpty()
                    ? QStringLiteral("This display is unavailable.")
                    : target.unavailableReason;
            }
            else if (target.current) {
                targetState.enabled = false;
                targetState.disabledCode = QStringLiteral("current_target");
                targetState.disabledReason =
                    QStringLiteral("This display is currently active.");
            }
            if (m_TransitionCoordinator != nullptr) {
                targetState = m_TransitionCoordinator->decorateTargetState(
                    target, std::move(targetState));
            }
            result.actionStates.insert(displayActionId(target),
                                       std::move(targetState));
        }
    }
    result.actionStates.insert(QStringLiteral("display.switch"),
                               std::move(displayTemplate));
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
    const PolarisDiscoverySnapshot discovery = discoverySnapshot();
    if (actionId.startsWith(QString::fromLatin1(DisplayPrefix))) {
        const PolarisAvailability displayAvailability =
            PolarisModels::availability(discovery,
                                        PolarisOperation::DisplaySwitch);
        const auto target = std::find_if(
            discovery.settings.targets.cbegin(),
            discovery.settings.targets.cend(),
            [&actionId](const DisplayTarget& candidate) {
                return displayActionId(candidate) == actionId;
            });
        const QVariantMap expectedMetadata = parameters.take(
            QStringLiteral("_perigee.authoritative-state")).toMap();
        const QVariantMap expectedCatalogIdentity = parameters.take(
            QStringLiteral("_perigee.authoritative-context")).toMap();
        if (target == discovery.settings.targets.cend() ||
                expectedMetadata != displayMetadata(*target) ||
                expectedCatalogIdentity.isEmpty() ||
                expectedCatalogIdentity !=
                    DisplayTransaction::catalogIdentity(discovery)) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("stale_catalog"),
                    QStringLiteral("The display target catalog changed."));
                if (target != discovery.settings.targets.cend()) {
                    ActionState observed = actionState(displayAvailability);
                    observed.value = displayMetadata(*target);
                    observed.invocationState =
                        DisplayTransaction::catalogIdentity(discovery);
                    result.observedState = std::move(observed);
                }
                completion(result);
            }
            return;
        }
        if (m_TransitionCoordinator == nullptr) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("transition_unavailable"),
                    QStringLiteral("Display switching is unavailable in this session."));
                ActionState observed = actionState(displayAvailability);
                observed.value = expectedMetadata;
                result.observedState = std::move(observed);
                completion(result);
            }
            return;
        }
        m_TransitionCoordinator->selectTarget(
            expectedMetadata, expectedCatalogIdentity, discovery,
            std::move(completion));
        return;
    }
    if (actionId.startsWith(QStringLiteral("host.command."))) {
        const PolarisAvailability commandAvailability =
            PolarisModels::availability(discovery,
                                        PolarisOperation::NamedCommand);
        if (!commandAvailability.enabled) {
            if (completion) {
                ActionResult result = unavailableAction(commandAvailability);
                result.observedState = actionState(commandAvailability);
                completion(result);
            }
            return;
        }
        const QString suffix = actionId.mid(
            QStringLiteral("host.command.").size());
        bool indexOk = false;
        const int index = suffix.toInt(&indexOk);
        if (!indexOk || index < 0 || QString::number(index) != suffix) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("action_unavailable"),
                    QStringLiteral("Action is unavailable."));
                result.observedState = ActionState{};
                completion(result);
            }
            return;
        }
        const auto command = std::find_if(
            discovery.capabilities.commands.cbegin(),
            discovery.capabilities.commands.cend(),
            [index](const NamedCommandMetadata& candidate) {
                return candidate.index == index;
            });
        if (command == discovery.capabilities.commands.cend() ||
                !command->endpoint.usable) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("stale_catalog"),
                    QStringLiteral("The host command catalog changed."));
                result.observedState = actionState(commandAvailability);
                completion(result);
            }
            return;
        }
        const QVariantMap expectedMetadata = parameters.take(
            QStringLiteral("_perigee.authoritative-state")).toMap();
        const QVariantMap currentMetadata{
            {QStringLiteral("index"), command->index},
            {QStringLiteral("name"), command->displayName},
            {QStringLiteral("risk"), command->risk},
        };
        if (expectedMetadata != currentMetadata) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("stale_catalog"),
                    QStringLiteral("The host command catalog changed."));
                ActionState observed = actionState(commandAvailability);
                observed.disruptive = command->risk != QStringLiteral("safe");
                observed.value = currentMetadata;
                result.observedState = std::move(observed);
                completion(result);
            }
            return;
        }
        ActionState authoritativeState = actionState(commandAvailability);
        authoritativeState.disruptive =
            command->risk != QStringLiteral("safe");
        authoritativeState.value = currentMetadata;
        submitAction(
            actionId, QStringLiteral("host.command"),
            command->endpoint.advertised,
            compactObject({
                {QStringLiteral("index"), index},
                {QStringLiteral("session_token"),
                 discovery.session.sessionToken},
            }),
            std::nullopt,
            ActionKind::Command, std::move(authoritativeState),
            std::move(completion));
        return;
    }
    if (actionId == QStringLiteral("session.end-host")) {
        const PolarisAvailability stopAvailability =
            PolarisModels::availability(discovery,
                                        PolarisOperation::StopSession);
        if (!stopAvailability.enabled) {
            if (completion) {
                ActionResult result = unavailableAction(stopAvailability);
                result.observedState = actionState(stopAvailability);
                completion(result);
            }
            return;
        }
        submitAction(
            actionId, QStringLiteral("session.lifecycle"),
            discovery.session.controls.stopEndpoint.advertised,
            compactObject({
                {QStringLiteral("session_token"),
                 discovery.session.sessionToken},
            }),
            std::nullopt,
            ActionKind::StopSession, actionState(stopAvailability),
            std::move(completion));
        return;
    }
    if (actionId == QStringLiteral("clipboard.send-local")) {
        const PolarisAvailability availability = PolarisModels::availability(
            discovery, PolarisOperation::ClipboardWrite);
        ActionState observed = actionState(availability);
        if (!availability.enabled) {
            if (completion) {
                ActionResult result = unavailableAction(availability);
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        if (QThread::currentThreadId() != m_State->sdlThread) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("wrong_thread"),
                    QStringLiteral("Clipboard access is unavailable on this thread."));
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        QByteArray text;
        if (m_Clipboard == nullptr || !m_Clipboard->readText(&text)) {
            wipeSensitive(text, m_Clipboard.get());
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("clipboard_unavailable"),
                    QStringLiteral("The local clipboard is unavailable."));
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        if (!strictUtf8(text)) {
            wipeSensitive(text, m_Clipboard.get());
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("invalid_utf8"),
                    QStringLiteral("The local clipboard is not valid UTF-8 text."));
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        const qint64 limit = std::min(
            discovery.capabilities.maxClipboardTextBytes,
            PolarisModels::AbsoluteClipboardTextCeiling);
        if (text.size() > limit) {
            wipeSensitive(text, m_Clipboard.get());
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("clipboard_too_large"),
                    QStringLiteral("The clipboard exceeds the host text limit."));
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        submitAction(actionId, QStringLiteral("clipboard"), {},
                     std::move(text), limit, ActionKind::ClipboardSend,
                     std::move(observed), std::move(completion));
        return;
    }
    if (actionId == QStringLiteral("clipboard.fetch-remote")) {
        const PolarisAvailability availability = PolarisModels::availability(
            discovery, PolarisOperation::ClipboardRead);
        ActionState observed = actionState(availability);
        if (!availability.enabled) {
            if (completion) {
                ActionResult result = unavailableAction(availability);
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        if (QThread::currentThreadId() != m_State->sdlThread) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("wrong_thread"),
                    QStringLiteral("Clipboard access is unavailable on this thread."));
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        const qint64 limit = std::min(
            discovery.capabilities.maxClipboardTextBytes,
            PolarisModels::AbsoluteClipboardTextCeiling);
        submitAction(actionId, QStringLiteral("clipboard"), {}, {}, limit,
                     ActionKind::ClipboardFetch, std::move(observed),
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
    const std::shared_ptr<SharedState> state = m_State;
    const std::shared_ptr<PolarisTransport> transport = m_Transport;
    const std::shared_ptr<PolarisClipboard> clipboard = m_Clipboard;
    QVector<PolarisTransport::RequestId> requests;
    QVector<QByteArray> sensitiveBuffers;
    {
        QMutexLocker locker(&state->mutex);
        for (auto it = state->actions.begin();
             it != state->actions.end(); ++it) {
            if (!it.value()->finished &&
                    it.value()->resourceKey == resourceKey) {
                sensitiveBuffers.push_back(
                    std::move(it.value()->sensitiveBytes));
                if (it.value()->earlyResponse.has_value()) {
                    sensitiveBuffers.push_back(
                        std::move(it.value()->earlyResponse->body));
                }
                requests.push_back(it.key());
            }
        }
    }
    for (QByteArray& bytes : sensitiveBuffers) {
        wipeSensitive(bytes, clipboard.get());
    }
    if (transport != nullptr) {
        for (const PolarisTransport::RequestId request : requests) {
            transport->cancel(request);
        }
    }
}

void PolarisAdapter::submitAction(
    const QString& actionId, const QString& resourceKey,
    const QString& endpoint, QByteArray body,
    std::optional<qint64> clipboardLimit, ActionKind kind,
    ActionState authoritativeState, Completion completion)
{
    const std::shared_ptr<SharedState> state = m_State;
    const std::shared_ptr<PolarisTransport> transport = m_Transport;
    const std::shared_ptr<PolarisClipboard> clipboard = m_Clipboard;
    if (transport == nullptr) {
        wipeSensitive(body, clipboard.get());
        if (completion) {
            ActionResult result = actionFailure(
                QStringLiteral("polaris_unreachable"),
                QStringLiteral("Polaris is unreachable"));
            result.observedState = authoritativeState;
            completion(result);
        }
        return;
    }
    const auto request = std::make_shared<ActionRequest>();
    request->actionId = actionId;
    request->resourceKey = resourceKey;
    request->kind = kind;
    request->clipboardLimit = clipboardLimit;
    request->authoritativeState = std::move(authoritativeState);
    if (kind == ActionKind::ClipboardSend) {
        request->sensitiveBytes = std::move(body);
    }
    request->completion = std::move(completion);
    const std::weak_ptr<SharedState> weakState = state;
    PolarisTransport::Completion transportCompletion =
        [weakState, request](PolarisTransport::RequestId deliveredRequestId,
                             const PolarisResponse& response) {
        handleActionCompletion(weakState, request, deliveredRequestId,
                               response);
    };
    PolarisTransport::RequestId requestId = 0;
    if (kind == ActionKind::ClipboardSend) {
        requestId = transport->sendClipboard(
            request->sensitiveBytes, std::move(transportCompletion));
    }
    else if (kind == ActionKind::ClipboardFetch) {
        requestId = transport->fetchClipboard(
            clipboardLimit, std::move(transportCompletion));
    }
    else {
        requestId = transport->post(
            endpoint, body, kind != ActionKind::DisplayTarget,
            std::move(transportCompletion));
    }

    std::optional<PolarisResponse> earlyResponse;
    PolarisTransport::RequestId earlyRequestId = 0;
    bool cancelRequest = false;
    QVector<QByteArray> discardedSensitiveBuffers;
    {
        QMutexLocker locker(&state->mutex);
        request->requestId = requestId;
        request->registered = true;
        if (!state->alive) {
            request->finished = true;
            request->completion = {};
            discardedSensitiveBuffers.push_back(
                std::move(request->sensitiveBytes));
            if (request->earlyResponse.has_value()) {
                discardedSensitiveBuffers.push_back(
                    std::move(request->earlyResponse->body));
            }
            request->earlyResponse.reset();
            cancelRequest = true;
        }
        else {
            state->actions.insert(requestId, request);
            earlyResponse = std::move(request->earlyResponse);
            earlyRequestId = request->earlyRequestId;
            request->earlyResponse.reset();
        }
    }
    if (cancelRequest) {
        for (QByteArray& bytes : discardedSensitiveBuffers) {
            wipeSensitive(bytes, clipboard.get());
        }
        transport->cancel(requestId);
    }
    else if (earlyResponse.has_value()) {
        ScopedSensitiveWipe responseWipe(
            earlyResponse->body, clipboard);
        handleActionCompletion(weakState, request, earlyRequestId,
                               *earlyResponse);
    }
}

void PolarisAdapter::handleActionCompletion(
    const std::weak_ptr<SharedState>& weakState,
    const std::shared_ptr<ActionRequest>& request,
    PolarisTransport::RequestId requestId, const PolarisResponse& response)
{
    const std::shared_ptr<SharedState> state = weakState.lock();
    if (!state) {
        return;
    }

    Completion completion;
    ActionKind kind = ActionKind::Command;
    std::shared_ptr<PolarisClipboard> clipboard;
    Qt::HANDLE sdlThread = nullptr;
    {
        QMutexLocker locker(&state->mutex);
        if (!request->registered) {
            if (!request->earlyResponse.has_value()) {
                request->earlyRequestId = requestId;
                request->earlyResponse = response;
            }
            return;
        }
        if (!state->alive || request->finished ||
                requestId != request->requestId ||
                state->actions.value(requestId) != request) {
            return;
        }
        request->finished = true;
        state->actions.remove(requestId);
        completion = std::move(request->completion);
        kind = request->kind;
        clipboard = state->clipboard;
        sdlThread = state->sdlThread;
    }

    ActionResult result;
    const int status = response.httpStatus;
    const bool transportFailure =
        !response.errorCode.isEmpty() &&
        response.errorCode != QStringLiteral("http_error");
    if (transportFailure) {
        const QString code =
            (kind == ActionKind::ClipboardSend ||
             kind == ActionKind::ClipboardFetch) &&
                (response.errorCode == QStringLiteral("response_too_large") ||
                 response.errorCode == QStringLiteral("request_too_large"))
            ? QStringLiteral("clipboard_too_large")
            : response.errorCode;
        result = actionFailure(
            code,
            response.userMessage.isEmpty()
                ? QStringLiteral("Polaris request failed.")
                : response.userMessage);
    }
    else if (status == 400) {
        result = actionFailure(QStringLiteral("invalid_request"),
                               QStringLiteral("Polaris rejected the request."));
    }
    else if (status == 401) {
        result = actionFailure(QStringLiteral("authentication_failed"),
                               QStringLiteral("Polaris authentication failed."));
    }
    else if (status == 403) {
        result = actionFailure(QStringLiteral("permission_denied"),
                               QStringLiteral("This paired client lacks permission."));
    }
    else if (status == 409) {
        result = actionFailure(QStringLiteral("stale_session"),
                               QStringLiteral("The host session changed."));
    }
    else if (status == 470 && kind == ActionKind::StopSession) {
        result = actionFailure(QStringLiteral("session_not_owned"),
                               QStringLiteral("This client does not own the host session."));
    }
    else if (status == 500) {
        result = actionFailure(QStringLiteral("host_operation_failed"),
                               QStringLiteral("The host operation failed."));
    }
    else if (status == 413 &&
            (kind == ActionKind::ClipboardSend ||
             kind == ActionKind::ClipboardFetch)) {
        result = actionFailure(QStringLiteral("clipboard_too_large"),
                               QStringLiteral("The clipboard exceeds the host text limit."));
    }
    else if (!response.errorCode.isEmpty()) {
        result = actionFailure(
            response.errorCode,
            response.userMessage.isEmpty()
                ? QStringLiteral("Polaris request failed.")
                : response.userMessage);
    }
    else if (!response.authenticated) {
        result = actionFailure(QStringLiteral("malformed_response"),
                               QStringLiteral("Polaris returned an invalid response."));
    }
    else if (kind == ActionKind::ClipboardSend) {
        if (status >= 200 && status < 300) {
            result = {true, QStringLiteral("Clipboard sent to host"), {}, {}};
        }
        else {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned an invalid clipboard response."));
        }
    }
    else if (kind == ActionKind::ClipboardFetch) {
        QByteArray text = response.body;
        if (status < 200 || status >= 300) {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned an invalid clipboard response."));
        }
        else if (request->clipboardLimit.has_value() &&
                 text.size() > *request->clipboardLimit) {
            result = actionFailure(
                QStringLiteral("clipboard_too_large"),
                QStringLiteral("The clipboard exceeds the host text limit."));
        }
        else if (!strictUtf8(text)) {
            result = actionFailure(
                QStringLiteral("invalid_utf8"),
                QStringLiteral("The host clipboard is not valid UTF-8 text."));
        }
        else if (QThread::currentThreadId() != sdlThread) {
            result = actionFailure(
                QStringLiteral("wrong_thread"),
                QStringLiteral("Clipboard access is unavailable on this thread."));
        }
        else if (clipboard == nullptr || !clipboard->writeText(text)) {
            result = actionFailure(
                QStringLiteral("clipboard_unavailable"),
                QStringLiteral("The local clipboard is unavailable."));
        }
        else {
            result = {true, QStringLiteral("Clipboard copied from host"), {}, {}};
        }
        wipeSensitive(text, clipboard.get());
    }
    else if (kind == ActionKind::DisplayTarget) {
        if (status >= 200 && status < 300) {
            result = {true, QStringLiteral("Display selection accepted by Polaris"),
                      {}, {}};
        }
        else {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned an invalid display response."));
        }
    }
    else if (!response.json.isObject()) {
        result = actionFailure(QStringLiteral("malformed_response"),
                               QStringLiteral("Polaris returned an invalid response."));
    }
    else if (kind == ActionKind::Command) {
        const QJsonObject object = response.json.object();
        if (status == 202 &&
                object.value(QStringLiteral("accepted")) == true &&
                object.value(QStringLiteral("state")) ==
                    QStringLiteral("accepted")) {
            result = {true, QStringLiteral("Accepted by Polaris"), {}, {}};
            QMutexLocker locker(&state->mutex);
            if (state->alive) {
                state->refreshRequested = true;
            }
        }
        else {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned an invalid command response."));
        }
    }
    else {
        const QJsonObject object = response.json.object();
        if (status >= 200 && status < 300 &&
                object.value(QStringLiteral("status")) == true) {
            result = {true, QStringLiteral("Host session end accepted"), {}, {}};
        }
        else {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned an invalid session response."));
        }
    }
    wipeSensitive(request->sensitiveBytes, clipboard.get());
    if (completion) {
        result.observedState = request->authoritativeState;
        completion(result);
    }
}
