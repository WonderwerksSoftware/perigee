#include "actionregistry.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <atomic>
#include <memory>
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
        current.disabledReason = QStringLiteral(
                "The connected host does not advertise this capability.");
        return current;
    }

    if ((snapshot.grantedPermissions & descriptor.requiredPermissions)
            != descriptor.requiredPermissions) {
        current.enabled = false;
        current.disabledReason = QStringLiteral("This paired client lacks permission.");
    }
    return current;
}

}

struct ActionRegistry::RuntimeState {
    struct Progress {
        ActionPhase phase = ActionPhase::Idle;
        QString message;
    };

    QMutex mutex;
    QHash<QString, Progress> progressByAction;
    QHash<QString, QString> actionByResource;
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

    ActionState current = evaluateSnapshotState(*descriptorIt, m_Adapter.snapshot());
    QMutexLocker locker(&m_RuntimeState->mutex);
    const auto progressIt = m_RuntimeState->progressByAction.constFind(actionId);
    if (progressIt != m_RuntimeState->progressByAction.cend()) {
        current.phase = progressIt->phase;
        current.message = progressIt->message;
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

bool ActionRegistry::requiresConfirmation(const QString& actionId, bool disruptive) const
{
    const auto descriptorIt = std::find_if(m_Descriptors.cbegin(), m_Descriptors.cend(),
                                           [&actionId](const ActionDescriptor& descriptor) {
        return descriptor.id == actionId;
    });
    if (descriptorIt == m_Descriptors.cend()) {
        return false;
    }

    switch (descriptorIt->confirmation) {
    case ConfirmationPolicy::Never:
        return false;
    case ConfirmationPolicy::Always:
        return true;
    case ConfirmationPolicy::WhenDisruptive:
        return disruptive;
    }
    return false;
}

void ActionRegistry::execute(const QString& actionId,
                             const QVariantMap& parameters,
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
            QStringLiteral("state_changed"),
            QStringLiteral("Action is unavailable."),
        };
        if (completion) {
            completion(result);
        }
        return;
    }

    const ActionState current = evaluateSnapshotState(*descriptorIt, m_Adapter.snapshot());
    if (!current.enabled) {
        const ActionResult result {
            false,
            {},
            QStringLiteral("state_changed"),
            current.disabledReason,
        };
        if (completion) {
            completion(result);
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
                actionId, { ActionPhase::Working, {} });
    }

    const QString resourceKey = descriptorIt->resourceKey;
    const std::weak_ptr<RuntimeState> weakRuntimeState = m_RuntimeState;
    const auto callbackUsed = std::make_shared<std::atomic_bool>(false);
    m_Adapter.execute(
            actionId,
            parameters,
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
            runtimeState->progressByAction.insert(
                    actionId,
                    { result.ok ? ActionPhase::Succeeded : ActionPhase::Failed,
                      result.ok ? result.evidence : result.userMessage });
        }
        if (completion) {
            completion(result);
        }
    });
}
