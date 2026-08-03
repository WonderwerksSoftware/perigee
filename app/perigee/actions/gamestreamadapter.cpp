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
    };
}

ActionResult mismatchResult(const QString& setting)
{
    return {
        false,
        {},
        QStringLiteral("state_mismatch"),
        QStringLiteral("%1 did not reach the requested state.").arg(setting),
    };
}

ActionResult stateResult(bool observed,
                         bool requested,
                         const QString& enabledEvidence,
                         const QString& disabledEvidence,
                         const QString& setting)
{
    if (observed != requested) {
        return mismatchResult(setting);
    }
    return {true, observed ? enabledEvidence : disabledEvidence, {}, {}};
}

bool requestedState(const QVariantMap& parameters, bool current)
{
    return parameters.contains(QStringLiteral("enabled"))
        ? parameters.value(QStringLiteral("enabled")).toBool()
        : !current;
}

bool isConfirmed(const QVariantMap& parameters)
{
    return parameters.value(QStringLiteral("confirmed"), false).toBool();
}

void complete(const HostAdapter::Completion& completion,
              const ActionResult& result)
{
    if (completion) {
        completion(result);
    }
}

}

GameStreamAdapter::GameStreamAdapter(SessionFacade* session)
    : m_Session(session)
    , m_LifetimeAuthority(session != nullptr ? session->lifetimeAuthority() : nullptr)
{
}

QVector<ActionDescriptor> GameStreamAdapter::descriptors()
{
    return {
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
    result.actionStates.insert(QString::fromLatin1(DisconnectClientId),
                               availableState());
    result.actionStates.insert(QString::fromLatin1(QuitPerigeeId),
                               availableState());
    return result;
}

void GameStreamAdapter::execute(const QString& actionId,
                                const QVariantMap& parameters,
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
        if (authority->mouseCaptureEnabled() || authority->keyboardCaptureEnabled()) {
            complete(completion, mismatchResult(QStringLiteral("Captured input")));
        }
        else {
            complete(completion,
                     {true,
                      QStringLiteral("Mouse and keyboard capture: released"),
                      {}, {}});
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
    if (actionId == QString::fromLatin1(DisconnectClientId)) {
        if (!isConfirmed(parameters)) {
            complete(completion,
                     {false, {}, QStringLiteral("confirmation_required"),
                      QStringLiteral("Confirm the client disconnect before continuing.")});
            return;
        }
        authority->requestClientDisconnect();
        complete(completion,
                 {true, QStringLiteral("Client disconnect requested"), {}, {}});
        return;
    }
    if (actionId == QString::fromLatin1(QuitPerigeeId)) {
        if (!isConfirmed(parameters)) {
            complete(completion,
                     {false, {}, QStringLiteral("confirmation_required"),
                      QStringLiteral("Confirm quitting Perigee before continuing.")});
            return;
        }
        authority->requestPerigeeQuit();
        complete(completion,
                 {true, QStringLiteral("Perigee quit requested"), {}, {}});
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
    return m_LifetimeAuthority.isNull() ? nullptr : m_Session;
}
