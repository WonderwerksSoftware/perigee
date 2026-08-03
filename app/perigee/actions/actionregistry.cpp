#include "actionregistry.h"

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

QString categoryName(ActionCategory category)
{
    switch (category) {
    case ActionCategory::Display:
        return QStringLiteral("display");
    case ActionCategory::Input:
        return QStringLiteral("input");
    case ActionCategory::Clipboard:
        return QStringLiteral("clipboard");
    case ActionCategory::Stats:
        return QStringLiteral("stats");
    case ActionCategory::Window:
        return QStringLiteral("window");
    case ActionCategory::Session:
        return QStringLiteral("session");
    }
    return {};
}

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
    if (categoryName(descriptor.category).contains(query, Qt::CaseInsensitive)) {
        return 4;
    }
    return -1;
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
            left.disabledReason == right.disabledReason;
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
    QVector<ActionDescriptor> matches;
    for (const ActionDescriptor& descriptor : m_Descriptors) {
        if (descriptor.category == category) {
            matches.push_back(descriptor);
        }
    }
    return matches;
}

QVector<ActionDescriptor> ActionRegistry::search(const QString& query) const
{
    const QString normalizedQuery = query.trimmed();
    if (normalizedQuery.isEmpty()) {
        return m_Descriptors;
    }

    struct RankedDescriptor {
        ActionDescriptor descriptor;
        int rank;
    };
    QVector<RankedDescriptor> ranked;
    for (const ActionDescriptor& descriptor : m_Descriptors) {
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
    const auto descriptorIt = std::find_if(m_Descriptors.cbegin(), m_Descriptors.cend(),
                                           [&actionId](const ActionDescriptor& descriptor) {
        return descriptor.id == actionId;
    });
    if (descriptorIt == m_Descriptors.cend()) {
        ActionState unavailable;
        unavailable.disabledReason = QStringLiteral("Action is unavailable.");
        return unavailable;
    }

    for (;;) {
        quint64 snapshotRevision;
        {
            QMutexLocker locker(&m_RuntimeState->mutex);
            snapshotRevision = m_RuntimeState->renderRevision;
        }

        ActionState current = evaluateSnapshotState(*descriptorIt, m_Adapter.snapshot());
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
        if (!descriptorIt->resourceKey.isEmpty()
                && m_RuntimeState->actionByResource.contains(descriptorIt->resourceKey)) {
            current.enabled = false;
            if (m_RuntimeState->actionByResource.value(descriptorIt->resourceKey) == actionId) {
                current.disabledReason = QStringLiteral("This action is already in progress.");
            } else {
                current.disabledReason = QStringLiteral(
                        "Another action for this resource is already in progress.");
            }
        }
        return current;
    }
}

bool ActionRegistry::requiresConfirmation(const QString& actionId) const
{
    const auto descriptorIt = std::find_if(m_Descriptors.cbegin(), m_Descriptors.cend(),
                                           [&actionId](const ActionDescriptor& descriptor) {
        return descriptor.id == actionId;
    });
    if (descriptorIt == m_Descriptors.cend()) {
        return false;
    }

    const ActionState current = evaluateSnapshotState(
        *descriptorIt, m_Adapter.snapshot());
    return confirmationRequired(*descriptorIt, current);
}

bool ActionRegistry::beginConfirmation(const QString& actionId)
{
    cancelConfirmation();

    const auto descriptorIt = std::find_if(m_Descriptors.cbegin(), m_Descriptors.cend(),
                                           [&actionId](const ActionDescriptor& descriptor) {
        return descriptor.id == actionId;
    });
    if (descriptorIt == m_Descriptors.cend()) {
        return false;
    }
    const ActionState current = evaluateSnapshotState(
        *descriptorIt, m_Adapter.snapshot());
    if (!current.enabled || !confirmationRequired(*descriptorIt, current)) {
        return false;
    }

    QMutexLocker locker(&m_RuntimeState->mutex);
    m_RuntimeState->pendingConfirmation =
        RuntimeState::PendingConfirmation {actionId, current};
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
    const auto descriptorIt = std::find_if(m_Descriptors.cbegin(), m_Descriptors.cend(),
                                           [&actionId](const ActionDescriptor& descriptor) {
        return descriptor.id == actionId;
    });
    if (descriptorIt == m_Descriptors.cend()) {
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

    const ActionState current = evaluateSnapshotState(*descriptorIt, m_Adapter.snapshot());
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

    if (confirmationRequired(*descriptorIt, current) &&
            !invocation.confirmationGrantedFor(actionId)) {
        if (completion) {
            completion({false, {}, QStringLiteral("confirmation_required"),
                        QStringLiteral("Confirm this action before continuing.")});
        }
        return;
    }

    {
        QMutexLocker locker(&m_RuntimeState->mutex);
        if (!descriptorIt->resourceKey.isEmpty()
                && m_RuntimeState->actionByResource.contains(descriptorIt->resourceKey)) {
            const bool sameAction = m_RuntimeState->actionByResource.value(
                    descriptorIt->resourceKey) == actionId;
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
        if (!descriptorIt->resourceKey.isEmpty()) {
            m_RuntimeState->actionByResource.insert(descriptorIt->resourceKey, actionId);
        }
        m_RuntimeState->progressByAction.insert(
                actionId, { ActionPhase::Working, {}, std::nullopt });
        ++m_RuntimeState->renderRevision;
    }

    const QString resourceKey = descriptorIt->resourceKey;
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
