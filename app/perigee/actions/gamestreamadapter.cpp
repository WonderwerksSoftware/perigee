#include "gamestreamadapter.h"

#include "sessionfacade.h"

#include <QObject>

namespace {

constexpr auto MouseCaptureId = "input.mouse-capture";
constexpr auto KeyboardCaptureId = "input.keyboard-capture";
constexpr auto ReleaseCapturedId = "input.release-captured";
constexpr auto StatsOverlayId = "stats.overlay";
constexpr auto FullscreenId = "window.fullscreen";
constexpr auto DisconnectClientId = "session.disconnect-client";
constexpr auto QuitPerigeeId = "session.quit-perigee";
constexpr auto PhysicalDisplayStatusId = "display.physical-status";
constexpr auto PhysicalDisplayPrefix = "display.physical.";
constexpr auto PhysicalDisplayResource = "display.physical";
constexpr int MinimumPhysicalDisplay = 1;
constexpr int MaximumPhysicalDisplay = 13;

ActionDescriptor descriptor(const char* id,
                            const char* label,
                            ActionCategory category,
                            const QStringList& aliases = {},
                            const char* resourceKey = "",
                            ConfirmationPolicy confirmation = ConfirmationPolicy::Never,
                            const char* confirmationMessage = "")
{
    return {
        QString::fromLatin1(id),
        QString::fromLatin1(label),
        category,
        aliases,
        QString::fromLatin1(resourceKey),
        {},
        0,
        confirmation,
        QString::fromLatin1(confirmationMessage),
    };
}

ActionState availableState(const QVariant& value = {})
{
    ActionState state;
    state.enabled = true;
    state.value = value;
    return state;
}

ActionState unavailableState()
{
    ActionState state;
    state.disabledCode = QStringLiteral("session_unavailable");
    state.disabledReason = QStringLiteral("The streaming session is unavailable.");
    return state;
}

ActionResult unavailableResult()
{
    return {
        false,
        {},
        QStringLiteral("session_unavailable"),
        QStringLiteral("The streaming session is unavailable."),
        unavailableState(),
    };
}

ActionResult requestRejectedResult(const QString& operation,
                                   ActionState observedState)
{
    return {
        false,
        {},
        QStringLiteral("request_rejected"),
        QStringLiteral("%1 could not be queued.").arg(operation),
        std::move(observedState),
    };
}

ActionResult mismatchResult(
    const QString& setting,
    std::optional<ActionState> observedState = std::nullopt)
{
    return {
        false,
        {},
        QStringLiteral("state_mismatch"),
        QStringLiteral("%1 did not reach the requested state.").arg(setting),
        std::move(observedState),
    };
}

ActionResult stateResult(bool observed,
                         bool requested,
                         const QString& enabledEvidence,
                         const QString& disabledEvidence,
                         const QString& setting)
{
    ActionState observedState = availableState(observed);
    if (observed != requested) {
        return mismatchResult(setting, std::move(observedState));
    }
    return {true, observed ? enabledEvidence : disabledEvidence, {}, {},
            std::move(observedState)};
}

bool requestedState(const QVariantMap& parameters, bool current)
{
    return parameters.contains(QStringLiteral("enabled"))
        ? parameters.value(QStringLiteral("enabled")).toBool()
        : !current;
}

void complete(const HostAdapter::Completion& completion,
              const ActionResult& result)
{
    if (completion) {
        completion(result);
    }
}

QString physicalDisplayId(int displayNumber)
{
    return QString::fromLatin1(PhysicalDisplayPrefix) +
        QString::number(displayNumber);
}

bool physicalDisplayNumber(const QString& actionId, int* displayNumber)
{
    const QString prefix = QString::fromLatin1(PhysicalDisplayPrefix);
    if (!actionId.startsWith(prefix)) {
        return false;
    }
    const QString suffix = actionId.mid(prefix.size());
    if (suffix.isEmpty() ||
            (suffix.size() > 1 && suffix.startsWith(QLatin1Char('0')))) {
        return false;
    }
    for (const QChar character : suffix) {
        if (character < QLatin1Char('0') ||
                character > QLatin1Char('9')) {
            return false;
        }
    }
    bool parsed = false;
    const int number = suffix.toInt(&parsed);
    if (!parsed || number < MinimumPhysicalDisplay ||
            number > MaximumPhysicalDisplay) {
        return false;
    }
    if (displayNumber != nullptr) {
        *displayNumber = number;
    }
    return true;
}

ActionState physicalDisplayState(SessionFacade* authority, int displayNumber)
{
    if (authority == nullptr) {
        return unavailableState();
    }

    const QPointer<SessionFacade> guardedAuthority(authority);
    const int displayCount = qBound(
        MinimumPhysicalDisplay, authority->physicalDisplayCount(),
        MaximumPhysicalDisplay);
    if (guardedAuthority.isNull()) {
        return unavailableState();
    }
    const int lastRequested = authority->lastRequestedPhysicalDisplay();
    if (guardedAuthority.isNull()) {
        return unavailableState();
    }
    const bool switchActive = authority->physicalDisplaySwitchActive();
    if (guardedAuthority.isNull()) {
        return unavailableState();
    }

    ActionState state;
    state.visible = displayNumber <= displayCount;
    state.enabled = state.visible && !switchActive;
    if (displayNumber == lastRequested) {
        state.value = QStringLiteral("Last requested");
    }
    if (state.visible && switchActive) {
        state.disabledCode = QStringLiteral("display_request_in_progress");
        state.disabledReason = QStringLiteral(
            "A physical display request is in progress.");
    }
    return state;
}

ActionState physicalDisplayStatusState(SessionFacade* authority)
{
    if (authority == nullptr) {
        return unavailableState();
    }

    const QPointer<SessionFacade> guardedAuthority(authority);
    const int lastRequested = authority->lastRequestedPhysicalDisplay();
    if (guardedAuthority.isNull()) {
        return unavailableState();
    }

    ActionState state;
    state.value = lastRequested == 0
        ? QStringLiteral("Unknown")
        : QStringLiteral("Last requested: Display %1")
              .arg(lastRequested);
    state.disabledCode = QStringLiteral("informational");
    state.disabledReason = QStringLiteral(
        "The host does not report an authoritative active physical display.");
    return state;
}

}

GameStreamAdapter::GameStreamAdapter(SessionFacade* session)
    : m_Session(session)
{
}

QVector<ActionDescriptor> GameStreamAdapter::descriptors()
{
    QVector<ActionDescriptor> result {
        descriptor(MouseCaptureId, "Mouse capture", ActionCategory::Input,
                   {QStringLiteral("mouse"), QStringLiteral("grab")}, "input.mouse"),
        descriptor(KeyboardCaptureId, "Keyboard capture", ActionCategory::Input,
                   {QStringLiteral("keyboard"), QStringLiteral("system keys")}, "input.keyboard"),
        descriptor(ReleaseCapturedId, "Release captured input", ActionCategory::Input,
                   {QStringLiteral("release"), QStringLiteral("ungrab")}, "input.capture"),
        descriptor(StatsOverlayId, "Streaming statistics", ActionCategory::Stats,
                   {QStringLiteral("performance"), QStringLiteral("debug")}, "stats"),
        descriptor(FullscreenId, "Fullscreen", ActionCategory::Window,
                   {QStringLiteral("windowed"), QStringLiteral("screen")}, "window.mode"),
        descriptor(
            DisconnectClientId,
            "Disconnect client",
            ActionCategory::Session,
            {QStringLiteral("disconnect"), QStringLiteral("leave stream")},
            "session.lifecycle",
            ConfirmationPolicy::Always,
            "Disconnect this client? Perigee will stay open, and the host session will continue."),
        descriptor(
            QuitPerigeeId,
            "Quit Perigee",
            ActionCategory::Session,
            {QStringLiteral("quit"), QStringLiteral("exit client")},
            "session.lifecycle",
            ConfirmationPolicy::Always,
            "Quit Perigee? The client will close, and the host session will continue."),
    };

    result.push_back(descriptor(
        PhysicalDisplayStatusId, "Physical display", ActionCategory::Display,
        {QStringLiteral("display status"), QStringLiteral("monitor status")}));
    for (int displayNumber = MinimumPhysicalDisplay;
         displayNumber <= MaximumPhysicalDisplay; ++displayNumber) {
        result.push_back({
            physicalDisplayId(displayNumber),
            QStringLiteral("Display %1").arg(displayNumber),
            ActionCategory::Display,
            {QStringLiteral("display"), QStringLiteral("monitor"),
             QStringLiteral("screen"), QString::number(displayNumber)},
            QString::fromLatin1(PhysicalDisplayResource),
            {}, 0, ConfirmationPolicy::Never, {},
        });
    }
    return result;
}

HostSnapshot GameStreamAdapter::snapshot()
{
    HostSnapshot result;
    SessionFacade* authority = session();
    for (const ActionDescriptor& action : descriptors()) {
        result.actionStates.insert(action.id, unavailableState());
    }
    if (authority == nullptr) {
        return result;
    }

    result.actionStates.insert(QString::fromLatin1(MouseCaptureId),
                               availableState(authority->mouseCaptureEnabled()));
    result.actionStates.insert(QString::fromLatin1(KeyboardCaptureId),
                               availableState(authority->keyboardCaptureEnabled()));
    result.actionStates.insert(
        QString::fromLatin1(ReleaseCapturedId),
        availableState(!authority->mouseCaptureEnabled() &&
                       !authority->keyboardCaptureEnabled()));
    result.actionStates.insert(QString::fromLatin1(StatsOverlayId),
                               availableState(authority->statsOverlayEnabled()));
    result.actionStates.insert(QString::fromLatin1(FullscreenId),
                               availableState(authority->fullscreenEnabled()));
    authority = session();
    if (authority == nullptr) {
        return result;
    }
    result.actionStates.insert(QString::fromLatin1(DisconnectClientId),
                               availableState());
    result.actionStates.insert(QString::fromLatin1(QuitPerigeeId),
                               availableState());
    result.actionStates.insert(QString::fromLatin1(PhysicalDisplayStatusId),
                               physicalDisplayStatusState(authority));
    for (int displayNumber = MinimumPhysicalDisplay;
         displayNumber <= MaximumPhysicalDisplay; ++displayNumber) {
        authority = session();
        if (authority == nullptr) {
            return result;
        }
        result.actionStates.insert(
            physicalDisplayId(displayNumber),
            physicalDisplayState(authority, displayNumber));
    }
    return result;
}

void GameStreamAdapter::execute(const QString& actionId,
                                QVariantMap parameters,
                                Completion completion)
{
    SessionFacade* authority = session();
    if (authority == nullptr) {
        complete(completion, unavailableResult());
        return;
    }

    if (actionId == QString::fromLatin1(StatsOverlayId)) {
        const bool requested = requestedState(parameters,
                                              authority->statsOverlayEnabled());
        authority->setStatsOverlayEnabled(requested);
        complete(completion,
                 stateResult(authority->statsOverlayEnabled(), requested,
                             QStringLiteral("Statistics overlay: on"),
                             QStringLiteral("Statistics overlay: off"),
                             QStringLiteral("Statistics overlay")));
        return;
    }
    if (actionId == QString::fromLatin1(MouseCaptureId)) {
        const bool requested = requestedState(parameters,
                                              authority->mouseCaptureEnabled());
        authority->setMouseCaptureEnabled(requested);
        complete(completion,
                 stateResult(authority->mouseCaptureEnabled(), requested,
                             QStringLiteral("Mouse capture: on"),
                             QStringLiteral("Mouse capture: off"),
                             QStringLiteral("Mouse capture")));
        return;
    }
    if (actionId == QString::fromLatin1(KeyboardCaptureId)) {
        const bool requested = requestedState(parameters,
                                              authority->keyboardCaptureEnabled());
        authority->setKeyboardCaptureEnabled(requested);
        complete(completion,
                 stateResult(authority->keyboardCaptureEnabled(), requested,
                             QStringLiteral("Keyboard capture: on"),
                             QStringLiteral("Keyboard capture: off"),
                             QStringLiteral("Keyboard capture")));
        return;
    }
    if (actionId == QString::fromLatin1(ReleaseCapturedId)) {
        authority->setMouseCaptureEnabled(false);
        authority->setKeyboardCaptureEnabled(false);
        const bool released = !authority->mouseCaptureEnabled() &&
                !authority->keyboardCaptureEnabled();
        ActionState observedState = availableState(released);
        if (!released) {
            complete(completion,
                     mismatchResult(QStringLiteral("Captured input"),
                                    std::move(observedState)));
        }
        else {
            complete(completion,
                     {true,
                      QStringLiteral("Mouse and keyboard capture: released"),
                      {}, {}, std::move(observedState)});
        }
        return;
    }
    if (actionId == QString::fromLatin1(FullscreenId)) {
        const bool requested = requestedState(parameters,
                                              authority->fullscreenEnabled());
        authority->setFullscreenEnabled(requested);
        complete(completion,
                 stateResult(authority->fullscreenEnabled(), requested,
                             QStringLiteral("Window mode: fullscreen"),
                             QStringLiteral("Window mode: windowed"),
                             QStringLiteral("Window mode")));
        return;
    }
    int displayNumber = 0;
    if (physicalDisplayNumber(actionId, &displayNumber)) {
        const QPointer<SessionFacade> guardedAuthority(authority);
        const bool accepted = authority->requestPhysicalDisplay(
            displayNumber,
            [guardedAuthority, displayNumber,
             completion](
                const ActionResult& completed) mutable {
                ActionResult observedResult = completed;
                observedResult.observedState = physicalDisplayState(
                    guardedAuthority.data(), displayNumber);
                complete(completion, observedResult);
            });
        if (!accepted) {
            ActionResult rejected {
                false, {},
                QStringLiteral("request_rejected"),
                QStringLiteral(
                    "The physical display request could not be queued."),
                physicalDisplayState(authority, displayNumber),
            };
            complete(completion, rejected);
        }
        return;
    }
    if (actionId == QString::fromLatin1(DisconnectClientId)) {
        if (authority->requestClientDisconnect()) {
            complete(completion,
                     {true, QStringLiteral("Client disconnect requested"), {}, {},
                      availableState()});
        }
        else {
            complete(completion,
                     requestRejectedResult(QStringLiteral("Client disconnect"),
                                           availableState()));
        }
        return;
    }
    if (actionId == QString::fromLatin1(QuitPerigeeId)) {
        if (authority->requestPerigeeQuit()) {
            complete(completion,
                     {true, QStringLiteral("Perigee quit requested"), {}, {},
                      availableState()});
        }
        else {
            complete(completion,
                     requestRejectedResult(QStringLiteral("Perigee quit"),
                                           availableState()));
        }
        return;
    }

    complete(completion,
             {false, {}, QStringLiteral("action_unavailable"),
              QStringLiteral("Action is unavailable.")});
}

void GameStreamAdapter::cancel(const QString&)
{
    // Local operations complete synchronously and have no cancellable work.
}

SessionFacade* GameStreamAdapter::session() const
{
    return m_Session.data();
}
