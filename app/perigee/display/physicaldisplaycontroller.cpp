#include "physicaldisplaycontroller.h"

#include <utility>

PhysicalDisplayController::PhysicalDisplayController(Clock clock,
                                                     ShortcutSender sender)
    : m_Clock(std::move(clock))
    , m_Sender(std::move(sender))
{
}

bool PhysicalDisplayController::request(int displayNumber,
                                        Completion completion)
{
    if (displayNumber < MinimumDisplay || displayNumber > MaximumDisplay
            || !completion || !m_Clock || !m_Sender || active()) {
        return false;
    }

    m_Completion = std::move(completion);
    m_RequestedDisplay = displayNumber;
    m_PreviousDisplay = m_LastRequestedDisplay;
    m_RestorationAttempted = false;

    if (sendAndArm(displayNumber, Phase::WaitingForFrame)) {
        return true;
    }

    disarmFrameGate();
    m_Completion = {};
    m_RequestedDisplay = 0;
    m_PreviousDisplay = 0;
    m_Deadline = 0;
    m_EvidenceEpoch = 0;
    m_Phase = Phase::Failed;
    return false;
}

bool PhysicalDisplayController::notifyAcceptedFrame(const Enqueue& enqueue)
{
    if (!enqueue) {
        return false;
    }

    quint64 gate = m_FrameGate.load(std::memory_order_acquire);
    while (gate != 0 && (gate & QueuedBit) == 0) {
        const quint64 queuedGate = gate | QueuedBit;
        if (!m_FrameGate.compare_exchange_weak(
                    gate, queuedGate,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            continue;
        }

        if (enqueue(gate)) {
            return true;
        }

        quint64 expected = queuedGate;
        m_FrameGate.compare_exchange_strong(
            expected, gate,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        return false;
    }

    return false;
}

bool PhysicalDisplayController::observeFreshFrame(quint64 evidenceEpoch)
{
    if (!active() || evidenceEpoch == 0
            || evidenceEpoch != m_EvidenceEpoch) {
        return false;
    }

    const quint64 gate = m_FrameGate.load(std::memory_order_acquire);
    if (gate != (evidenceEpoch | QueuedBit)) {
        return false;
    }

    disarmFrameGate();

    if (m_Phase == Phase::WaitingForFrame) {
        const int requested = m_RequestedDisplay;
        m_LastRequestedDisplay = requested;
        finish(ActionResult {
            true,
            QStringLiteral("Display %1 requested; video resumed").arg(requested),
            {},
            {},
        });
        return true;
    }

    if (m_Phase == Phase::Restoring) {
        finish(ActionResult {
            false,
            {},
            QStringLiteral("display_switch_failed_restored"),
            QStringLiteral("Perigee did not verify the requested display. Video resumed after Perigee requested the previous display."),
        });
        return true;
    }

    return false;
}

bool PhysicalDisplayController::checkDeadline()
{
    if (!active() || !m_Clock || m_Clock() < m_Deadline) {
        return false;
    }

    disarmFrameGate();

    if (m_Phase == Phase::WaitingForFrame) {
        if (!m_RestorationAttempted && m_PreviousDisplay != 0
                && m_PreviousDisplay != m_RequestedDisplay) {
            m_RestorationAttempted = true;
            if (sendAndArm(m_PreviousDisplay, Phase::Restoring)) {
                return true;
            }

            finish(ActionResult {
                false,
                {},
                QStringLiteral("display_switch_and_restore_failed"),
                QStringLiteral("Perigee did not receive fresh video after the display request, and the restoration request could not be sent."),
            });
            return true;
        }

        finish(ActionResult {
            false,
            {},
            QStringLiteral("display_verification_timeout"),
            QStringLiteral("Perigee did not receive fresh video after the display request."),
        });
        return true;
    }

    if (m_Phase == Phase::Restoring) {
        finish(ActionResult {
            false,
            {},
            QStringLiteral("display_switch_and_restore_failed"),
            QStringLiteral("Perigee did not receive fresh video after the display request or the restoration request."),
        });
        return true;
    }

    return false;
}

void PhysicalDisplayController::cancel()
{
    if (!active()) {
        return;
    }

    finish(ActionResult {
        false,
        {},
        QStringLiteral("display_switch_cancelled"),
        QStringLiteral("The physical display request was cancelled."),
    });
}

int PhysicalDisplayController::lastRequestedDisplay() const
{
    return m_LastRequestedDisplay;
}

int PhysicalDisplayController::pendingDisplay() const
{
    if (m_Phase == Phase::Restoring) {
        return m_PreviousDisplay;
    }
    if (m_Phase == Phase::WaitingForFrame) {
        return m_RequestedDisplay;
    }
    return 0;
}

quint64 PhysicalDisplayController::evidenceEpoch() const
{
    return m_EvidenceEpoch;
}

bool PhysicalDisplayController::active() const
{
    return m_Phase == Phase::WaitingForFrame || m_Phase == Phase::Restoring;
}

PhysicalDisplayController::Phase PhysicalDisplayController::phase() const
{
    return m_Phase;
}

bool PhysicalDisplayController::sendAndArm(int displayNumber, Phase phase)
{
    if (!m_Sender(displayNumber)) {
        return false;
    }

    ++m_NextEvidenceEpoch;
    if (m_NextEvidenceEpoch == 0
            || m_NextEvidenceEpoch > MaximumEvidenceEpoch) {
        m_NextEvidenceEpoch = 1;
    }

    m_EvidenceEpoch = m_NextEvidenceEpoch;
    m_Deadline = m_Clock() + VerificationTimeoutMs;
    m_Phase = phase;
    m_FrameGate.store(m_EvidenceEpoch, std::memory_order_release);
    return true;
}

void PhysicalDisplayController::disarmFrameGate()
{
    m_FrameGate.store(0, std::memory_order_release);
}

void PhysicalDisplayController::finish(ActionResult result)
{
    disarmFrameGate();
    m_Phase = result.ok ? Phase::Succeeded : Phase::Failed;
    m_Deadline = 0;
    m_RequestedDisplay = 0;
    m_PreviousDisplay = 0;
    m_RestorationAttempted = false;
    m_EvidenceEpoch = 0;

    Completion completion = std::move(m_Completion);
    m_Completion = {};
    if (completion) {
        completion(result);
    }
}
