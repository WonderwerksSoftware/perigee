#include "perigee/display/physicaldisplaycontroller.h"
#include "test_registry.h"

#include <QtTest>

class PhysicalDisplayControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsNumbersOutsideOneThroughThirteen();
    void sendsOneRequestAndCompletesOnMatchingFreshFrame();
    void rejectsConcurrentRequests();
    void ignoresStaleFrameEvidence();
    void frameEnqueueFailureAllowsTheNextAcceptedFrame();
    void timesOutWithoutRestorationWhenInitialDisplayIsUnknown();
    void restoresOneKnownDisplayAfterTimeout();
    void reportsFailureWhenTheBoundedRestorationTimesOut();
    void cancellationCompletesOnceAndDisarmsTheRequest();
};

REGISTER_PERIGEE_TEST(PhysicalDisplayControllerTest);

namespace {

using Phase = PhysicalDisplayController::Phase;

struct Harness
{
    qint64 now = 1000;
    QVector<int> sends;
    QVector<ActionResult> completions;
    PhysicalDisplayController controller {
        [this] { return now; },
        [this](int displayNumber) {
            sends.push_back(displayNumber);
            return true;
        },
    };

    HostAdapter::Completion completion()
    {
        return [this](const ActionResult& result) {
            completions.push_back(result);
        };
    }

    quint64 enqueueFreshFrame()
    {
        quint64 epoch = 0;
        const bool queued = controller.notifyAcceptedFrame(
            [&epoch](quint64 evidenceEpoch) {
                epoch = evidenceEpoch;
                return true;
            });
        Q_ASSERT(queued);
        return epoch;
    }

    void completeSuccessfulRequest()
    {
        const quint64 epoch = enqueueFreshFrame();
        Q_ASSERT(controller.observeFreshFrame(epoch));
    }
};

}

void PhysicalDisplayControllerTest::rejectsNumbersOutsideOneThroughThirteen()
{
    Harness harness;

    QVERIFY(!harness.controller.request(0, harness.completion()));
    QVERIFY(!harness.controller.request(14, harness.completion()));
    QVERIFY(!harness.controller.active());
    QCOMPARE(harness.controller.pendingDisplay(), 0);
    QCOMPARE(harness.sends, QVector<int>());
    QCOMPARE(harness.completions.size(), 0);
}

void PhysicalDisplayControllerTest::sendsOneRequestAndCompletesOnMatchingFreshFrame()
{
    Harness harness;

    QVERIFY(harness.controller.request(2, harness.completion()));
    QCOMPARE(harness.sends, QVector<int>({2}));
    QVERIFY(harness.controller.active());
    QCOMPARE(harness.controller.phase(), Phase::WaitingForFrame);
    QCOMPARE(harness.controller.pendingDisplay(), 2);
    QVERIFY(harness.controller.evidenceEpoch() != 0);

    const quint64 epoch = harness.enqueueFreshFrame();
    QCOMPARE(epoch, harness.controller.evidenceEpoch());
    QVERIFY(harness.controller.observeFreshFrame(epoch));

    QCOMPARE(harness.completions.size(), 1);
    QVERIFY(harness.completions.first().ok);
    QCOMPARE(harness.completions.first().evidence,
             QStringLiteral("Display 2 requested; video resumed"));
    QVERIFY(!harness.controller.active());
    QCOMPARE(harness.controller.phase(), Phase::Succeeded);
    QCOMPARE(harness.controller.pendingDisplay(), 0);
    QCOMPARE(harness.controller.lastRequestedDisplay(), 2);
}

void PhysicalDisplayControllerTest::rejectsConcurrentRequests()
{
    Harness harness;
    int rejectedCompletionCount = 0;

    QVERIFY(harness.controller.request(1, harness.completion()));
    QVERIFY(!harness.controller.request(2, [&rejectedCompletionCount](const ActionResult&) {
        ++rejectedCompletionCount;
    }));

    QCOMPARE(harness.sends, QVector<int>({1}));
    QCOMPARE(rejectedCompletionCount, 0);
    QCOMPARE(harness.controller.pendingDisplay(), 1);
    harness.completeSuccessfulRequest();
    QCOMPARE(harness.completions.size(), 1);
}

void PhysicalDisplayControllerTest::ignoresStaleFrameEvidence()
{
    Harness harness;

    QVERIFY(harness.controller.request(3, harness.completion()));
    const quint64 currentEpoch = harness.controller.evidenceEpoch();
    QVERIFY(!harness.controller.observeFreshFrame(currentEpoch + 1));
    QVERIFY(harness.controller.active());
    QCOMPARE(harness.completions.size(), 0);

    const quint64 queuedEpoch = harness.enqueueFreshFrame();
    QCOMPARE(queuedEpoch, currentEpoch);
    QVERIFY(harness.controller.observeFreshFrame(queuedEpoch));
    QCOMPARE(harness.completions.size(), 1);
}

void PhysicalDisplayControllerTest::frameEnqueueFailureAllowsTheNextAcceptedFrame()
{
    Harness harness;
    int enqueueCalls = 0;
    quint64 queuedEpoch = 0;

    QVERIFY(!harness.controller.notifyAcceptedFrame([&](quint64) {
        ++enqueueCalls;
        return true;
    }));
    QCOMPARE(enqueueCalls, 0);

    QVERIFY(harness.controller.request(4, harness.completion()));
    QVERIFY(!harness.controller.notifyAcceptedFrame([&](quint64) {
        ++enqueueCalls;
        return false;
    }));
    QVERIFY(harness.controller.notifyAcceptedFrame([&](quint64 epoch) {
        ++enqueueCalls;
        queuedEpoch = epoch;
        return true;
    }));
    QVERIFY(!harness.controller.notifyAcceptedFrame([&](quint64) {
        ++enqueueCalls;
        return true;
    }));

    QCOMPARE(enqueueCalls, 2);
    QVERIFY(harness.controller.observeFreshFrame(queuedEpoch));
    QCOMPARE(harness.completions.size(), 1);
}

void PhysicalDisplayControllerTest::timesOutWithoutRestorationWhenInitialDisplayIsUnknown()
{
    Harness harness;

    QVERIFY(harness.controller.request(2, harness.completion()));
    harness.now += PhysicalDisplayController::VerificationTimeoutMs;
    QVERIFY(harness.controller.checkDeadline());

    QCOMPARE(harness.sends, QVector<int>({2}));
    QCOMPARE(harness.completions.size(), 1);
    QVERIFY(!harness.completions.first().ok);
    QCOMPARE(harness.completions.first().errorCode,
             QStringLiteral("display_verification_timeout"));
    QCOMPARE(harness.controller.phase(), Phase::Failed);
    QCOMPARE(harness.controller.lastRequestedDisplay(), 0);
    QVERIFY(!harness.controller.notifyAcceptedFrame([](quint64) {
        return true;
    }));
}

void PhysicalDisplayControllerTest::restoresOneKnownDisplayAfterTimeout()
{
    Harness harness;

    QVERIFY(harness.controller.request(1, harness.completion()));
    harness.completeSuccessfulRequest();
    harness.completions.clear();

    QVERIFY(harness.controller.request(2, harness.completion()));
    harness.now += PhysicalDisplayController::VerificationTimeoutMs;
    QVERIFY(harness.controller.checkDeadline());
    QCOMPARE(harness.controller.phase(), Phase::Restoring);
    QCOMPARE(harness.controller.pendingDisplay(), 1);
    QCOMPARE(harness.sends, QVector<int>({1, 2, 1}));
    QCOMPARE(harness.completions.size(), 0);

    harness.completeSuccessfulRequest();
    QCOMPARE(harness.completions.size(), 1);
    QVERIFY(!harness.completions.first().ok);
    QCOMPARE(harness.completions.first().errorCode,
             QStringLiteral("display_switch_failed_restored"));
    QCOMPARE(harness.controller.lastRequestedDisplay(), 1);
    QCOMPARE(harness.controller.phase(), Phase::Failed);
}

void PhysicalDisplayControllerTest::reportsFailureWhenTheBoundedRestorationTimesOut()
{
    Harness harness;

    QVERIFY(harness.controller.request(1, harness.completion()));
    harness.completeSuccessfulRequest();
    harness.completions.clear();

    QVERIFY(harness.controller.request(2, harness.completion()));
    harness.now += PhysicalDisplayController::VerificationTimeoutMs;
    QVERIFY(harness.controller.checkDeadline());
    harness.now += PhysicalDisplayController::VerificationTimeoutMs;
    QVERIFY(harness.controller.checkDeadline());

    QCOMPARE(harness.sends, QVector<int>({1, 2, 1}));
    QCOMPARE(harness.completions.size(), 1);
    QVERIFY(!harness.completions.first().ok);
    QCOMPARE(harness.completions.first().errorCode,
             QStringLiteral("display_switch_and_restore_failed"));
    QCOMPARE(harness.controller.lastRequestedDisplay(), 1);
    QCOMPARE(harness.controller.phase(), Phase::Failed);
}

void PhysicalDisplayControllerTest::cancellationCompletesOnceAndDisarmsTheRequest()
{
    Harness harness;

    QVERIFY(harness.controller.request(5, harness.completion()));
    const quint64 oldEpoch = harness.controller.evidenceEpoch();
    harness.controller.cancel();
    harness.controller.cancel();

    QCOMPARE(harness.completions.size(), 1);
    QVERIFY(!harness.completions.first().ok);
    QCOMPARE(harness.completions.first().errorCode,
             QStringLiteral("display_switch_cancelled"));
    QVERIFY(!harness.controller.active());
    QCOMPARE(harness.controller.phase(), Phase::Failed);
    QVERIFY(!harness.controller.notifyAcceptedFrame([](quint64) {
        return true;
    }));
    QVERIFY(!harness.controller.observeFreshFrame(oldEpoch));
    QCOMPARE(harness.completions.size(), 1);
}

#include "test_physicaldisplaycontroller.moc"
