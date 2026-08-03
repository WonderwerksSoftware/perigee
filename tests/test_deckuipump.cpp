#include "test_registry.h"

#include "perigee/deck/deckuipump.h"
#include "SDL_compat.h"

#include <QtTest>

class DeckUiPumpTest : public QObject
{
    Q_OBJECT

private slots:
    void coalescesDirtyBurstsAndKeepsTheFirstFrameImmediate();
    void ownsTheTextInputTransitionPolicy();
    void appliesTextInputTransitionsToSdl();
};

void DeckUiPumpTest::coalescesDirtyBurstsAndKeepsTheFirstFrameImmediate()
{
    qint64 now = 100;
    DeckUiPump pump([&now] { return now; });

    auto decision = pump.plan(true, true, true);
    QVERIFY(decision.render);
    for (int i = 0; i < 100; ++i) {
        QVERIFY(!pump.plan(true, true, true).render);
    }
    now = 115;
    QVERIFY(!pump.plan(true, true, true).render);
    now = 116;
    QVERIFY(pump.plan(true, true, true).render);
    QVERIFY(!pump.plan(true, true, false).render);

    pump.plan(false, false, true);
    QVERIFY(pump.plan(true, false, true).render);
}

void DeckUiPumpTest::ownsTheTextInputTransitionPolicy()
{
    qint64 now = 0;
    DeckUiPump pump([&now] { return now; });

    QCOMPARE(pump.plan(false, false, false).textInput,
             DeckUiPump::TextInputAction::NoChange);
    QCOMPARE(pump.plan(true, true, false).textInput,
             DeckUiPump::TextInputAction::Start);
    QCOMPARE(pump.plan(true, true, false).textInput,
             DeckUiPump::TextInputAction::NoChange);
    QCOMPARE(pump.plan(true, false, false).textInput,
             DeckUiPump::TextInputAction::Stop);
    QCOMPARE(pump.plan(true, true, false).textInput,
             DeckUiPump::TextInputAction::Start);
    QCOMPARE(pump.plan(false, true, false).textInput,
             DeckUiPump::TextInputAction::Stop);
}

void DeckUiPumpTest::appliesTextInputTransitionsToSdl()
{
    DeckUiPump pump;
    SDL_StopTextInput();
    QVERIFY(!SDL_IsTextInputActive());

    const auto start = pump.plan(true, true, false);
    pump.applyTextInput(start);
    QVERIFY(SDL_IsTextInputActive());

    const auto stop = pump.plan(false, false, false);
    pump.applyTextInput(stop);
    QVERIFY(!SDL_IsTextInputActive());
}

REGISTER_PERIGEE_TEST(DeckUiPumpTest);

#include "test_deckuipump.moc"
