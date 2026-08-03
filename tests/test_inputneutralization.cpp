#include "test_registry.h"

#include "streaming/input/remoteinputstate.h"

#include <QtTest>

class InputNeutralizationTest : public QObject
{
    Q_OBJECT

private slots:
    void neutralizationReleasesTrackedInputAndZerosEveryController();
    void doubleNeutralizationDoesNotDuplicateDiscreteReleases();
    void removalStopsZeroingFreedControllerSlot();
};

void InputNeutralizationTest::neutralizationReleasesTrackedInputAndZerosEveryController()
{
    RemoteInputState state;
    state.keySent(0x41, true);
    state.keySent(0xA2, true);
    state.keySent(0x41, true);
    state.mouseButtonSent(1, true);
    state.mouseButtonSent(4, true);
    state.controllerAllocated(0);
    state.controllerAllocated(3);

    const NeutralRemoteInput neutral = state.takeNeutralInput();
    QCOMPARE(neutral.keyReleases, QVector<short>({0x41, 0xA2}));
    QCOMPARE(neutral.mouseButtonReleases, QVector<int>({1, 4}));
    QCOMPARE(neutral.controllerIndicesToZero, QVector<short>({0, 3}));
    QVERIFY(!state.hasKeysDown());
    QVERIFY(!state.hasMouseButtonsDown());
}

void InputNeutralizationTest::doubleNeutralizationDoesNotDuplicateDiscreteReleases()
{
    RemoteInputState state;
    state.keySent(0x42, true);
    state.mouseButtonSent(3, true);
    state.controllerAllocated(1);

    const NeutralRemoteInput first = state.takeNeutralInput();
    const NeutralRemoteInput second = state.takeNeutralInput();
    QCOMPARE(first.keyReleases, QVector<short>({0x42}));
    QCOMPARE(first.mouseButtonReleases, QVector<int>({3}));
    QCOMPARE(first.controllerIndicesToZero, QVector<short>({1}));
    QVERIFY(second.keyReleases.isEmpty());
    QVERIFY(second.mouseButtonReleases.isEmpty());
    QCOMPARE(second.controllerIndicesToZero, QVector<short>({1}));

    state.keySent(0x42, false);
    state.mouseButtonSent(3, false);
    const NeutralRemoteInput third = state.takeNeutralInput();
    QVERIFY(third.keyReleases.isEmpty());
    QVERIFY(third.mouseButtonReleases.isEmpty());
}

void InputNeutralizationTest::removalStopsZeroingFreedControllerSlot()
{
    RemoteInputState state;
    state.controllerAllocated(2);
    state.controllerAllocated(7);
    state.controllerRemoved(2);

    QCOMPARE(state.takeNeutralInput().controllerIndicesToZero,
             QVector<short>({7}));
}

REGISTER_PERIGEE_TEST(InputNeutralizationTest);

#include "test_inputneutralization.moc"
