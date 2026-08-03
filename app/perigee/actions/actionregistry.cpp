#include "actionregistry.h"
#include "actioncategories.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr auto CommandTemplateId = "host.command";
constexpr auto CommandPrefix = "host.command.";
constexpr auto DisplayTemplateId = "display.switch";
constexpr auto DisplayPrefix = "display.target.";

int searchRank(const ActionDescriptor& descriptor, const QString& query)
{
    if (descriptor.label.startsWith(query, Qt::CaseInsensitive)) {
        return 0;
    }
    if (descriptor.label.contains(query, Qt::CaseInsensitive)) {
        return 1;
    }
    for (const QString& alias : descriptor.aliases) {
        if (alias.startsWith(query, Qt::CaseInsensitive)) {
            return 2;
        }
    }
    for (const QString& alias : descriptor.aliases) {
        if (alias.contains(query, Qt::CaseInsensitive)) {
            return 3;
        }
    }
    if (ActionCategories::displayName(descriptor.category).contains(
            query, Qt::CaseInsensitive)) {
        return 4;
    }
    return -1;
}

bool commandIndex(const QString& actionId, int* index)
{
    const QString prefix = QString::fromLatin1(CommandPrefix);
    if (!actionId.startsWith(prefix)) {
        return false;
    }
    const QString suffix = actionId.mid(prefix.size());
    if (suffix.isEmpty() ||
            (suffix.size() > 1 && suffix.startsWith(QLatin1Char('0')))) {
        return false;
    }
    for (const QChar character : suffix) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            return false;
        }
    }
    bool ok = false;
    const int parsed = suffix.toInt(&ok);
    if (!ok || parsed < 0) {
        return false;
    }
    if (index != nullptr) {
        *index = parsed;
    }
    return true;
}

QString displayActionId(const QString& kind, const QString& id)
{
    const QByteArray key = (kind + QLatin1Char(':') + id).toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(DisplayPrefix) + QString::fromLatin1(key);
}

bool displayMetadata(const QString& actionId, const ActionState& state,
                     QVariantMap* metadata, QString* stableKey)
{
    if (!actionId.startsWith(QString::fromLatin1(DisplayPrefix))) {
        return false;
    }
    const QVariantMap value = state.value.toMap();
    const QStringList required {
        QStringLiteral("kind"), QStringLiteral("id"), QStringLiteral("label"),
        QStringLiteral("available"), QStringLiteral("current"),
        QStringLiteral("requires_reconnect"),
        QStringLiteral("unavailable_reason"),
    };
    for (const QString& key : required) {
        if (!value.contains(key)) {
            return false;
        }
    }
    const QString kind = value.value(QStringLiteral("kind")).toString();
    const QString id = value.value(QStringLiteral("id")).toString();
    const QString label = value.value(QStringLiteral("label")).toString();
    if ((kind != QStringLiteral("output") &&
         kind != QStringLiteral("stream-mode")) ||
            id.isEmpty() || label.isEmpty() ||
            displayActionId(kind, id) != actionId) {
        return false;
    }
    if (metadata) {
        *metadata = value;
    }
    if (stableKey) {
        *stableKey = kind + QLatin1Char(':') + id;
    }
    return true;
}

QVector<ActionDescriptor> resolvedDescriptors(
    const QVector<ActionDescriptor>& templates, const HostSnapshot& snapshot)
{
    QVector<ActionDescriptor> result;
    std::optional<ActionDescriptor> commandTemplate;
    std::optional<ActionDescriptor> displayTemplate;
    for (const ActionDescriptor& descriptor : templates) {
        if (descriptor.id == QString::fromLatin1(CommandTemplateId)) {
            commandTemplate = descriptor;
        }
        else if (descriptor.id == QString::fromLatin1(DisplayTemplateId)) {
            displayTemplate = descriptor;
        }
        else {
            result.push_back(descriptor);
        }
    }

    if (displayTemplate.has_value()) {
        struct DynamicDisplay {
            QString stableKey;
            ActionDescriptor descriptor;
        };
        QVector<DynamicDisplay> displays;
        for (auto it = snapshot.actionStates.cbegin();
             it != snapshot.actionStates.cend(); ++it) {
            QVariantMap metadata;
            QString stableKey;
            if (!displayMetadata(it.key(), it.value(), &metadata, &stableKey)) {
                continue;
            }
            ActionDescriptor descriptor = *displayTemplate;
            descriptor.id = it.key();
            descriptor.label = metadata.value(QStringLiteral("label")).toString();
            descriptor.aliases = {
                QStringLiteral("display"), QStringLiteral("monitor"),
                QStringLiteral("screen"),
                metadata.value(QStringLiteral("kind")).toString(),
                metadata.value(QStringLiteral("id")).toString(),
            };
            displays.push_back({stableKey, std::move(descriptor)});
        }
        std::sort(displays.begin(), displays.end(),
                  [](const DynamicDisplay& left, const DynamicDisplay& right) {
            return left.stableKey < right.stableKey;
        });
        if (displays.isEmpty()) {
            result.push_back(*displayTemplate);
        }
        else {
            for (DynamicDisplay& display : displays) {
                result.push_back(std::move(display.descriptor));
            }
        }
    }

    if (!commandTemplate.has_value()) {
        return result;
    }

    struct DynamicCommand {
        int index = -1;
        ActionDescriptor descriptor;
    };
    QVector<DynamicCommand> commands;
    for (auto it = snapshot.actionStates.cbegin();
         it != snapshot.actionStates.cend(); ++it) {
        int index = -1;
        if (!commandIndex(it.key(), &index)) {
            continue;
        }
        const QVariantMap metadata = it.value().value.toMap();
        bool metadataIndexOk = false;
        const int metadataIndex = metadata.value(
            QStringLiteral("index")).toInt(&metadataIndexOk);
        if (!metadataIndexOk || metadataIndex != index ||
                !metadata.contains(QStringLiteral("name")) ||
                !metadata.contains(QStringLiteral("risk"))) {
            continue;
        }
        ActionDescriptor descriptor = *commandTemplate;
        descriptor.id = it.key();
        descriptor.label = metadata.value(QStringLiteral("name")).toString();
        descriptor.aliases = {QStringLiteral("command")};
        commands.push_back({index, std::move(descriptor)});
    }
    std::sort(commands.begin(), commands.end(),
              [](const DynamicCommand& left, const DynamicCommand& right) {
        return left.index < right.index;
    });
    for (DynamicCommand& command : commands) {
        result.push_back(std::move(command.descriptor));
    }
    return result;
}

std::optional<ActionDescriptor> resolveDescriptor(
    const QVector<ActionDescriptor>& templates, const HostSnapshot& snapshot,
    const QString& actionId)
{
    const QVector<ActionDescriptor> descriptors =
        resolvedDescriptors(templates, snapshot);
    const auto iterator = std::find_if(
        descriptors.cbegin(), descriptors.cend(),
        [&actionId](const ActionDescriptor& descriptor) {
            return descriptor.id == actionId;
        });
    return iterator == descriptors.cend()
        ? std::nullopt
        : std::optional<ActionDescriptor>(*iterator);
}

ActionState evaluateSnapshotState(const ActionDescriptor& descriptor,
                                  const HostSnapshot& snapshot)
{
    ActionState current = snapshot.actionStates.value(descriptor.id);
    if (!descriptor.requiredCapability.isEmpty()
            && !snapshot.advertisedCapabilities.contains(descriptor.requiredCapability)) {
        current.enabled = false;
        current.disabledCode = QStringLiteral("capability_unavailable");
        current.disabledReason = QStringLiteral(
                "The connected host does not advertise this capability.");
        return current;
    }

    if ((snapshot.grantedPermissions & descriptor.requiredPermissions)
            != descriptor.requiredPermissions) {
        current.enabled = false;
        current.disabledCode = QStringLiteral("permission_denied");
        current.disabledReason = QStringLiteral("This paired client lacks permission.");
    }
    return current;
}

bool confirmationRequired(const ActionDescriptor& descriptor,
                          const ActionState& state)
{
    switch (descriptor.confirmation) {
    case ConfirmationPolicy::Never:
        return false;
    case ConfirmationPolicy::Always:
        return true;
    case ConfirmationPolicy::WhenDisruptive:
        return state.disruptive;
    }
    return false;
}

bool sameAuthoritativeState(const ActionState& left, const ActionState& right)
{
    const auto sameValue = [](const QVariant& leftValue, const QVariant& rightValue) {
        if (leftValue.metaType() != rightValue.metaType()) {
            return false;
        }

        switch (leftValue.metaType().id()) {
        case QMetaType::Double: {
            const double leftNumber = leftValue.toDouble();
            const double rightNumber = rightValue.toDouble();
            return (std::isnan(leftNumber) && std::isnan(rightNumber))
                    || leftNumber == rightNumber;
        }
        case QMetaType::Float: {
            const float leftNumber = leftValue.toFloat();
            const float rightNumber = rightValue.toFloat();
            return (std::isnan(leftNumber) && std::isnan(rightNumber))
                    || leftNumber == rightNumber;
        }
        default:
            return leftValue == rightValue;
        }
    };

    return left.visible == right.visible &&
            left.enabled == right.enabled &&
            left.disruptive == right.disruptive &&
            sameValue(left.value, right.value) &&
            left.disabledCode == right.disabledCode &&
            left.disabledReason == right.disabledReason &&
            left.invocationState == right.invocationState;
}

}

struct ActionRegistry::RuntimeState {
    struct Progress {
        ActionPhase phase = ActionPhase::Idle;
        QString message;
        std::optional<ActionState> observedState;
    };

    struct PendingConfirmation {
        QString actionId;
        ActionState state;
    };

    QMutex mutex;
    QHash<QString, Progress> progressByAction;
    QHash<QString, QString> actionByResource;
    QHash<QString, ActionState> renderedStateByAction;
    std::optional<PendingConfirmation> pendingConfirmation;
    quint64 renderRevision = 0;
};

ActionRegistry::ActionRegistry(QVector<ActionDescriptor> descriptors, HostAdapter& adapter)
    : m_Descriptors(std::move(descriptors))
    , m_Adapter(adapter)
    , m_RuntimeState(std::make_shared<RuntimeState>())
{
}

QVector<ActionDescriptor> ActionRegistry::actions(ActionCategory category) const
{
    const HostSnapshot snapshot = m_Adapter.snapshot();
    const QVector<ActionDescriptor> descriptors =
        resolvedDescriptors(m_Descriptors, snapshot);
    QVector<ActionDescriptor> matches;
    for (const ActionDescriptor& descriptor : descriptors) {
        if (descriptor.category == category) {
            matches.push_back(descriptor);
        }
    }
    return matches;
}

QVector<ActionDescriptor> ActionRegistry::search(const QString& query) const
{
    const HostSnapshot snapshot = m_Adapter.snapshot();
    const QVector<ActionDescriptor> descriptors =
        resolvedDescriptors(m_Descriptors, snapshot);
    const QString normalizedQuery = query.trimmed();
    if (normalizedQuery.isEmpty()) {
        return descriptors;
    }

    struct RankedDescriptor {
        ActionDescriptor descriptor;
        int rank;
    };
    QVector<RankedDescriptor> ranked;
    for (const ActionDescriptor& descriptor : descriptors) {
        const int rank = searchRank(descriptor, normalizedQuery);
        if (rank >= 0) {
            ranked.push_back({ descriptor, rank });
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.rank < right.rank;
    });

    QVector<ActionDescriptor> matches;
    matches.reserve(ranked.size());
    for (const RankedDescriptor& match : ranked) {
        matches.push_back(match.descriptor);
    }
    return matches;
}

ActionState ActionRegistry::state(const QString& actionId)
{
    for (;;) {
        quint64 snapshotRevision;
        {
            QMutexLocker locker(&m_RuntimeState->mutex);
            snapshotRevision = m_RuntimeState->renderRevision;
        }

        const HostSnapshot snapshot = m_Adapter.snapshot();
        const std::optional<ActionDescriptor> descriptor =
            resolveDescriptor(m_Descriptors, snapshot, actionId);
        if (!descriptor.has_value()) {
            ActionState unavailable;
            unavailable.disabledReason = QStringLiteral("Action is unavailable.");
            return unavailable;
        }
        ActionState current = evaluateSnapshotState(*descriptor, snapshot);
        const ActionState authoritativeCurrent = current;
        QMutexLocker locker(&m_RuntimeState->mutex);
        if (snapshotRevision != m_RuntimeState->renderRevision) {
            continue;
        }

        auto progressIt = m_RuntimeState->progressByAction.find(actionId);
        if (progressIt != m_RuntimeState->progressByAction.end()) {
            const bool terminal = progressIt->phase == ActionPhase::Succeeded ||
                    progressIt->phase == ActionPhase::Failed;
            if (terminal && progressIt->observedState.has_value() &&
                    !sameAuthoritativeState(current, *progressIt->observedState)) {
                m_RuntimeState->progressByAction.erase(progressIt);
                ++m_RuntimeState->renderRevision;
            }
            else {
                current.phase = progressIt->phase;
                current.message = progressIt->message;
            }
        }
        if (!descriptor->resourceKey.isEmpty()
                && m_RuntimeState->actionByResource.contains(descriptor->resourceKey)) {
            current.enabled = false;
            if (m_RuntimeState->actionByResource.value(descriptor->resourceKey) == actionId) {
                current.disabledReason = QStringLiteral("This action is already in progress.");
            } else {
                current.disabledReason = QStringLiteral(
                        "Another action for this resource is already in progress.");
            }
        }
        m_RuntimeState->renderedStateByAction.insert(
            actionId, authoritativeCurrent);
        return current;
    }
}

bool ActionRegistry::requiresConfirmation(const QString& actionId) const
{
    const HostSnapshot snapshot = m_Adapter.snapshot();
    const std::optional<ActionDescriptor> descriptor =
        resolveDescriptor(m_Descriptors, snapshot, actionId);
    if (!descriptor.has_value()) {
        return false;
    }

    const ActionState current = evaluateSnapshotState(*descriptor, snapshot);
    return confirmationRequired(*descriptor, current);
}

bool ActionRegistry::beginConfirmation(const QString& actionId)
{
    cancelConfirmation();

    const HostSnapshot snapshot = m_Adapter.snapshot();
    const std::optional<ActionDescriptor> descriptor =
        resolveDescriptor(m_Descriptors, snapshot, actionId);
    if (!descriptor.has_value()) {
        return false;
    }
    const ActionState current = evaluateSnapshotState(*descriptor, snapshot);
    ActionState rendered = current;
    {
        QMutexLocker locker(&m_RuntimeState->mutex);
        const auto renderedIt =
            m_RuntimeState->renderedStateByAction.constFind(actionId);
        if (renderedIt != m_RuntimeState->renderedStateByAction.cend()) {
            rendered = *renderedIt;
        }
    }
    if (!current.enabled || !confirmationRequired(*descriptor, current)) {
        return false;
    }

    QMutexLocker locker(&m_RuntimeState->mutex);
    m_RuntimeState->pendingConfirmation =
        RuntimeState::PendingConfirmation {actionId, rendered};
    return true;
}

void ActionRegistry::cancelConfirmation()
{
    QMutexLocker locker(&m_RuntimeState->mutex);
    m_RuntimeState->pendingConfirmation.reset();
}

void ActionRegistry::acceptConfirmation(const QString& actionId,
                                        const QVariantMap& parameters,
                                        HostAdapter::Completion completion)
{
    std::optional<RuntimeState::PendingConfirmation> pendingConfirmation;
    {
        QMutexLocker locker(&m_RuntimeState->mutex);
        pendingConfirmation = std::move(m_RuntimeState->pendingConfirmation);
        m_RuntimeState->pendingConfirmation.reset();
    }
    if (!pendingConfirmation.has_value() ||
            pendingConfirmation->actionId != actionId) {
        if (completion) {
            completion({false, {}, QStringLiteral("confirmation_required"),
                        QStringLiteral("Confirm this action before continuing.")});
        }
        return;
    }

    executeInvocation(actionId,
                      ActionInvocation(parameters, actionId,
                                       std::move(pendingConfirmation->state)),
                      std::move(completion));
}

void ActionRegistry::execute(const QString& actionId,
                             const QVariantMap& parameters,
                             HostAdapter::Completion completion)
{
    cancelConfirmation();
    executeInvocation(actionId, ActionInvocation(parameters),
                      std::move(completion));
}

void ActionRegistry::executeInvocation(const QString& actionId,
                                       ActionInvocation invocation,
                                       HostAdapter::Completion completion)
{
    const HostSnapshot snapshot = m_Adapter.snapshot();
    const std::optional<ActionDescriptor> descriptor =
        resolveDescriptor(m_Descriptors, snapshot, actionId);
    if (!descriptor.has_value()) {
        const ActionResult result {
            false,
            {},
            QStringLiteral("action_unavailable"),
            QStringLiteral("Action is unavailable."),
        };
        if (completion) {
            completion(result);
        }
        return;
    }

    const ActionState current = evaluateSnapshotState(*descriptor, snapshot);
    ActionState rendered = current;
    {
        QMutexLocker locker(&m_RuntimeState->mutex);
        const auto renderedIt =
            m_RuntimeState->renderedStateByAction.constFind(actionId);
        if (renderedIt != m_RuntimeState->renderedStateByAction.cend()) {
            rendered = *renderedIt;
        }
    }
    if (invocation.confirmationGrantedFor(actionId) &&
            (!invocation.m_ConfirmedState.has_value() ||
             !sameAuthoritativeState(current, *invocation.m_ConfirmedState))) {
        if (completion) {
            completion({false, {}, QStringLiteral("state_changed"),
                        QStringLiteral("Action state changed after confirmation.")});
        }
        return;
    }

    if (!current.enabled) {
        const ActionResult result {
            false,
            {},
            current.disabledCode.isEmpty()
                ? QStringLiteral("state_changed")
                : current.disabledCode,
            current.disabledReason,
        };
        if (completion) {
            completion(result);
        }
        return;
    }

    if (confirmationRequired(*descriptor, current) &&
            !invocation.confirmationGrantedFor(actionId)) {
        if (completion) {
            completion({false, {}, QStringLiteral("confirmation_required"),
                        QStringLiteral("Confirm this action before continuing.")});
        }
        return;
    }

    {
        QMutexLocker locker(&m_RuntimeState->mutex);
        if (!descriptor->resourceKey.isEmpty()
                && m_RuntimeState->actionByResource.contains(descriptor->resourceKey)) {
            const bool sameAction = m_RuntimeState->actionByResource.value(
                    descriptor->resourceKey) == actionId;
            const ActionResult result {
                false,
                {},
                QStringLiteral("resource_busy"),
                sameAction
                        ? QStringLiteral("This action is already in progress.")
                        : QStringLiteral(
                                  "Another action for this resource is already in progress."),
            };
            locker.unlock();
            if (completion) {
                completion(result);
            }
            return;
        }
        if (!descriptor->resourceKey.isEmpty()) {
            m_RuntimeState->actionByResource.insert(descriptor->resourceKey, actionId);
        }
        m_RuntimeState->progressByAction.insert(
                actionId, { ActionPhase::Working, {}, std::nullopt });
        ++m_RuntimeState->renderRevision;
    }

    const QString resourceKey = descriptor->resourceKey;
    if (commandIndex(actionId, nullptr) ||
            displayMetadata(actionId, current, nullptr, nullptr)) {
        invocation.m_Parameters.insert(
            QStringLiteral("_perigee.authoritative-state"), rendered.value);
        invocation.m_Parameters.insert(
            QStringLiteral("_perigee.authoritative-context"),
            rendered.invocationState);
    }
    const std::weak_ptr<RuntimeState> weakRuntimeState = m_RuntimeState;
    const auto callbackUsed = std::make_shared<std::atomic_bool>(false);
    m_Adapter.execute(
            actionId,
            std::move(invocation.m_Parameters),
            [weakRuntimeState, callbackUsed, actionId, resourceKey,
             completion = std::move(completion)](const ActionResult& result) mutable {
        if (callbackUsed->exchange(true)) {
            return;
        }
        const std::shared_ptr<RuntimeState> runtimeState = weakRuntimeState.lock();
        if (!runtimeState) {
            return;
        }
        {
            QMutexLocker locker(&runtimeState->mutex);
            if (!resourceKey.isEmpty()
                    && runtimeState->actionByResource.value(resourceKey) == actionId) {
                runtimeState->actionByResource.remove(resourceKey);
            }
            if (result.observedState.has_value()) {
                runtimeState->progressByAction.insert(
                        actionId,
                        { result.ok ? ActionPhase::Succeeded : ActionPhase::Failed,
                          result.ok ? result.evidence : result.userMessage,
                          result.observedState });
            }
            else {
                runtimeState->progressByAction.remove(actionId);
            }
            ++runtimeState->renderRevision;
        }
        if (completion) {
            completion(result);
        }
    });
}
