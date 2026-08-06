#include "polarisadapter.h"

#include "backend/nvcomputer.h"

#include "perigee/actions/gamestreamadapter.h"

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
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

namespace {

constexpr auto CapabilitiesRoute = "/polaris/v1/capabilities";
constexpr auto SessionStatusRoute = "/polaris/v1/session/status";
constexpr auto ClientSettingsRoute = "/polaris/v1/client-settings";
constexpr auto BitrateRoute = "/polaris/v1/session/bitrate";
constexpr auto AdaptiveBitrateRoute = "/polaris/v1/session/adaptive-bitrate";
constexpr auto QualityManualId = "quality.mode.manual";
constexpr auto QualityAdaptiveId = "quality.mode.adaptive";
constexpr auto QualityDecreaseId = "quality.bitrate.decrease";
constexpr auto QualityIncreaseId = "quality.bitrate.increase";
constexpr auto QualityStatusId = "quality.status";
constexpr auto QualityResource = "quality.tuning";
constexpr int BitrateStepKbps = 5000;
constexpr int ProtocolMinimumBitrateKbps = 1000;
constexpr int ProtocolMaximumBitrateKbps = 300000;
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

QString bitrateValueText(int bitrateKbps)
{
    if (bitrateKbps <= 0) {
        return QStringLiteral("Unknown bitrate");
    }
    const QString value = bitrateKbps % 1000 == 0
        ? QString::number(bitrateKbps / 1000)
        : QString::number(bitrateKbps / 1000.0, 'f', 1);
    return QStringLiteral("%1 Mbps").arg(value);
}

std::optional<int> qualityBaseBitrate(const PolarisClientSettings& settings)
{
    if (settings.adaptiveBaseBitrateKbps.has_value()) {
        return settings.adaptiveBaseBitrateKbps;
    }
    if (settings.encoderBitrateKbps.has_value()) {
        return settings.encoderBitrateKbps;
    }
    return settings.adaptiveTargetBitrateKbps;
}

bool exactJsonInt(const QJsonValue& value, int expected)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number &&
        number == expected;
}

std::optional<int> displayedQualityBitrate(
    const PolarisClientSettings& settings)
{
    if (settings.adaptiveBitrateEnabled.value_or(false) &&
            settings.adaptiveTargetBitrateKbps.has_value()) {
        return settings.adaptiveTargetBitrateKbps;
    }
    if (settings.encoderBitrateKbps.has_value()) {
        return settings.encoderBitrateKbps;
    }
    return qualityBaseBitrate(settings);
}

ActionState qualityStateUnavailable(const QString& reason)
{
    ActionState state;
    state.disabledCode = QStringLiteral("quality_state_unavailable");
    state.disabledReason = reason;
    return state;
}

ActionState qualityMutationState(const PolarisAvailability& availability,
                                 const PolarisClientSettings& settings,
                                 const QString& actionId)
{
    ActionState state;
    state.enabled = availability.enabled;
    state.disabledCode = availability.errorCode;
    state.disabledReason = availability.reason;
    if (!state.enabled) {
        return state;
    }
    if (actionId == QString::fromLatin1(QualityManualId) ||
            actionId == QString::fromLatin1(QualityAdaptiveId)) {
        if (!settings.adaptiveBitrateEnabled.has_value()) {
            return qualityStateUnavailable(
                QStringLiteral("Polaris did not report the quality mode."));
        }
        const bool adaptive = *settings.adaptiveBitrateEnabled;
        const bool selected = actionId == QString::fromLatin1(QualityAdaptiveId)
            ? adaptive : !adaptive;
        if (selected) {
            state.value = QStringLiteral("Selected");
        }
        return state;
    }

    const std::optional<int> current = qualityBaseBitrate(settings);
    if (!current.has_value()) {
        return qualityStateUnavailable(
            QStringLiteral("Polaris did not report a bitrate target."));
    }
    const int minimum = qBound(
        ProtocolMinimumBitrateKbps,
        settings.adaptiveMinBitrateKbps.value_or(
            ProtocolMinimumBitrateKbps),
        ProtocolMaximumBitrateKbps);
    const int maximum = qBound(
        minimum,
        settings.adaptiveMaxBitrateKbps.value_or(
            ProtocolMaximumBitrateKbps),
        ProtocolMaximumBitrateKbps);
    state.value = bitrateValueText(*current);
    if ((actionId == QString::fromLatin1(QualityDecreaseId) &&
         *current <= minimum) ||
            (actionId == QString::fromLatin1(QualityIncreaseId) &&
             *current >= maximum)) {
        state.enabled = false;
        state.disabledCode = QStringLiteral("bitrate_limit");
        state.disabledReason = QStringLiteral(
            "The bitrate is at the host limit.");
    }
    return state;
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
    std::unique_ptr<PolarisClipboard> clipboard)
    : m_LocalAdapter(localAdapter)
    , m_Transport(std::move(transport))
    , m_Clipboard(clipboard != nullptr
          ? std::shared_ptr<PolarisClipboard>(std::move(clipboard))
          : std::make_shared<SdlClipboard>())
    , m_State(std::make_shared<SharedState>())
{
    m_State->sdlThread = QThread::currentThreadId();
    m_State->clipboard = m_Clipboard;
    if (m_Transport != nullptr) {
        m_State->origin = m_Transport->pairedOrigin();
    }
}

PolarisAdapter::PolarisAdapter(GameStreamAdapter& localAdapter,
                               const NvComputer& computer)
    : PolarisAdapter(localAdapter,
                     std::make_unique<ApiClientTransport>(computer), {})
{
    m_DiscoveryEnabled = computer.isPolarisServerSoftware;
    if (!m_DiscoveryEnabled) {
        QMutexLocker locker(&m_State->mutex);
        m_State->published.complete = true;
        m_State->published.standardHost = true;
    }
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
    result.push_back(polarisDescriptor(
        QualityManualId, "Manual quality", ActionCategory::Quality,
        QualityResource, ConfirmationPolicy::Never));
    result.push_back(polarisDescriptor(
        QualityAdaptiveId, "Adaptive quality", ActionCategory::Quality,
        QualityResource, ConfirmationPolicy::Never));
    result.push_back(polarisDescriptor(
        QualityDecreaseId, "Reduce by 5 Mbps", ActionCategory::Quality,
        QualityResource, ConfirmationPolicy::Never));
    result.push_back(polarisDescriptor(
        QualityIncreaseId, "Increase by 5 Mbps", ActionCategory::Quality,
        QualityResource, ConfirmationPolicy::Never));
    return result;
}

bool PolarisAdapter::startDiscovery()
{
    if (!m_DiscoveryEnabled) {
        return false;
    }
    return beginGeneration(m_State, m_Transport, true);
}

bool PolarisAdapter::refresh()
{
    if (!m_DiscoveryEnabled) {
        return false;
    }
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
    const PolarisSessionStatus& session = completed.session;
    PolarisClientSettings& settings = completed.settings;
    if (session.adaptiveBitrateEnabled.has_value()) {
        settings.adaptiveBitrateEnabled = session.adaptiveBitrateEnabled;
    }
    if (session.aiAutoQualityEnabled.has_value()) {
        settings.aiAutoQualityEnabled = session.aiAutoQualityEnabled;
    }
    if (session.aiOptimizerEnabled.has_value()) {
        settings.aiOptimizerEnabled = session.aiOptimizerEnabled;
    }
    if (session.adaptiveTargetBitrateKbps.has_value()) {
        settings.adaptiveTargetBitrateKbps =
            session.adaptiveTargetBitrateKbps;
    }
    if (session.adaptiveBaseBitrateKbps.has_value()) {
        settings.adaptiveBaseBitrateKbps = session.adaptiveBaseBitrateKbps;
    }
    if (session.adaptiveMinBitrateKbps.has_value()) {
        settings.adaptiveMinBitrateKbps = session.adaptiveMinBitrateKbps;
    }
    if (session.adaptiveMaxBitrateKbps.has_value()) {
        settings.adaptiveMaxBitrateKbps = session.adaptiveMaxBitrateKbps;
    }
    if (session.encoderBitrateKbps.has_value()) {
        settings.encoderBitrateKbps = session.encoderBitrateKbps;
    }
    if (!session.adaptiveState.isEmpty()) {
        settings.adaptiveState = session.adaptiveState;
    }
    if (!session.adaptiveReason.isEmpty()) {
        settings.adaptiveReason = session.adaptiveReason;
    }
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
    const PolarisAvailability bitrateAvailability =
        PolarisModels::availability(
            discovery, PolarisOperation::BitrateControl);
    const PolarisAvailability adaptiveAvailability =
        PolarisModels::availability(
            discovery, PolarisOperation::AdaptiveQualityControl);
    result.actionStates.insert(
        QString::fromLatin1(QualityManualId),
        qualityMutationState(adaptiveAvailability, discovery.settings,
                             QString::fromLatin1(QualityManualId)));
    result.actionStates.insert(
        QString::fromLatin1(QualityAdaptiveId),
        qualityMutationState(adaptiveAvailability, discovery.settings,
                             QString::fromLatin1(QualityAdaptiveId)));
    result.actionStates.insert(
        QString::fromLatin1(QualityDecreaseId),
        qualityMutationState(bitrateAvailability, discovery.settings,
                             QString::fromLatin1(QualityDecreaseId)));
    result.actionStates.insert(
        QString::fromLatin1(QualityIncreaseId),
        qualityMutationState(bitrateAvailability, discovery.settings,
                             QString::fromLatin1(QualityIncreaseId)));
    if (!discovery.standardHost) {
        ActionState status;
        status.disabledCode = QStringLiteral("informational");
        status.disabledReason = discovery.settings.adaptiveReason.isEmpty()
            ? QStringLiteral("Polaris reports the live stream quality.")
            : discovery.settings.adaptiveReason;
        const std::optional<int> bitrate = displayedQualityBitrate(
            discovery.settings);
        if (!discovery.settings.valid ||
                !discovery.settings.adaptiveBitrateEnabled.has_value() ||
                !bitrate.has_value()) {
            status = qualityStateUnavailable(
                QStringLiteral("Polaris did not report live quality state."));
            status.value = QStringLiteral("Unavailable");
        }
        else {
            const QString mode = *discovery.settings.adaptiveBitrateEnabled
                ? QStringLiteral("Adaptive") : QStringLiteral("Manual");
            status.value = QStringLiteral("%1 · %2")
                .arg(mode, bitrateValueText(*bitrate));
        }
        result.actionStates.insert(
            QString::fromLatin1(QualityStatusId), std::move(status));
    }
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
    if (actionId == QString::fromLatin1(QualityManualId) ||
            actionId == QString::fromLatin1(QualityAdaptiveId)) {
        const PolarisAvailability availability = PolarisModels::availability(
            discovery, PolarisOperation::AdaptiveQualityControl);
        ActionState observed = qualityMutationState(
            availability, discovery.settings, actionId);
        if (!observed.enabled) {
            if (completion) {
                ActionResult result = actionFailure(
                    observed.disabledCode.isEmpty()
                        ? QStringLiteral("action_unavailable")
                        : observed.disabledCode,
                    observed.disabledReason.isEmpty()
                        ? QStringLiteral("Quality control is unavailable.")
                        : observed.disabledReason);
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        const bool enabled = actionId ==
            QString::fromLatin1(QualityAdaptiveId);
        observed.invocationState.insert(QStringLiteral("enabled"), enabled);
        submitAction(
            actionId, QString::fromLatin1(QualityResource),
            QString::fromLatin1(AdaptiveBitrateRoute),
            compactObject({{QStringLiteral("enabled"), enabled}}),
            std::nullopt, ActionKind::QualityMode, std::move(observed),
            std::move(completion));
        return;
    }
    if (actionId == QString::fromLatin1(QualityDecreaseId) ||
            actionId == QString::fromLatin1(QualityIncreaseId)) {
        const PolarisAvailability availability = PolarisModels::availability(
            discovery, PolarisOperation::BitrateControl);
        ActionState observed = qualityMutationState(
            availability, discovery.settings, actionId);
        if (!observed.enabled) {
            if (completion) {
                ActionResult result = actionFailure(
                    observed.disabledCode.isEmpty()
                        ? QStringLiteral("action_unavailable")
                        : observed.disabledCode,
                    observed.disabledReason.isEmpty()
                        ? QStringLiteral("Bitrate control is unavailable.")
                        : observed.disabledReason);
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        const std::optional<int> current = qualityBaseBitrate(
            discovery.settings);
        if (!current.has_value()) {
            if (completion) {
                ActionResult result = actionFailure(
                    QStringLiteral("quality_state_unavailable"),
                    QStringLiteral("Polaris did not report a bitrate target."));
                result.observedState = observed;
                completion(result);
            }
            return;
        }
        const int minimum = qBound(
            ProtocolMinimumBitrateKbps,
            discovery.settings.adaptiveMinBitrateKbps.value_or(
                ProtocolMinimumBitrateKbps),
            ProtocolMaximumBitrateKbps);
        const int maximum = qBound(
            minimum,
            discovery.settings.adaptiveMaxBitrateKbps.value_or(
                ProtocolMaximumBitrateKbps),
            ProtocolMaximumBitrateKbps);
        const int delta = actionId == QString::fromLatin1(QualityIncreaseId)
            ? BitrateStepKbps : -BitrateStepKbps;
        const int requested = std::clamp(*current + delta, minimum, maximum);
        observed.invocationState.insert(
            QStringLiteral("bitrate_kbps"), requested);
        submitAction(
            actionId, QString::fromLatin1(QualityResource),
            QString::fromLatin1(BitrateRoute),
            compactObject({{QStringLiteral("bitrate_kbps"), requested}}),
            std::nullopt, ActionKind::Bitrate, std::move(observed),
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
            endpoint, body, true, std::move(transportCompletion));
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
    else if (!response.json.isObject()) {
        result = actionFailure(QStringLiteral("malformed_response"),
                               QStringLiteral("Polaris returned an invalid response."));
    }
    else if (kind == ActionKind::QualityMode) {
        const QJsonObject object = response.json.object();
        const bool expected = request->authoritativeState.invocationState
            .value(QStringLiteral("enabled")).toBool();
        const QJsonValue adaptive = object.value(
            QStringLiteral("adaptive_bitrate_enabled"));
        const QJsonValue automatic = object.value(
            QStringLiteral("ai_auto_quality_enabled"));
        const QJsonValue optimizer = object.value(
            QStringLiteral("ai_optimizer_enabled"));
        if (status >= 200 && status < 300 &&
                object.value(QStringLiteral("status")) == true &&
                adaptive.isBool() && adaptive.toBool() == expected &&
                automatic.isBool() && automatic.toBool() == expected &&
                optimizer.isBool() && optimizer.toBool() == expected) {
            result = {true,
                      expected ? QStringLiteral("Adaptive quality accepted")
                               : QStringLiteral("Manual quality accepted"),
                      {}, {}};
            QMutexLocker locker(&state->mutex);
            if (state->alive) {
                state->refreshRequested = true;
            }
        }
        else {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned invalid quality state."));
        }
    }
    else if (kind == ActionKind::Bitrate) {
        const QJsonObject object = response.json.object();
        const int expected = request->authoritativeState.invocationState
            .value(QStringLiteral("bitrate_kbps")).toInt();
        if (status >= 200 && status < 300 &&
                object.value(QStringLiteral("status")) == true &&
                exactJsonInt(object.value(QStringLiteral("bitrate_kbps")),
                             expected)) {
            result = {true, QStringLiteral("Bitrate request accepted"), {}, {}};
            QMutexLocker locker(&state->mutex);
            if (state->alive) {
                state->refreshRequested = true;
            }
        }
        else {
            result = actionFailure(
                QStringLiteral("malformed_response"),
                QStringLiteral("Polaris returned an invalid bitrate response."));
        }
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
