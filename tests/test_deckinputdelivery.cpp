#include "test_registry.h"

#include "perigee/input/deckinputdelivery.h"
#include "perigee/actions/actionregistry.h"
#include "perigee/deck/deckcontroller.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtTest>

namespace {

class OpenHostAdapter final : public HostAdapter
{
public:
    HostSnapshot snapshot() override
    {
        HostSnapshot snapshot;
        ActionState state;
        state.enabled = true;
        snapshot.actionStates.insert(QStringLiteral("display.select"), state);
        return snapshot;
    }

    void execute(const QString&, QVariantMap, Completion) override {}
    void cancel(const QString&) override {}
};

class RecordingSink final : public DeckInputSink
{
public:
    bool sendKeyEvent(QKeyEvent* event) override
    {
        ++keyEvents;
        keyTexts.append(event->text());
        return true;
    }

    bool sendPointerEvent(QMouseEvent*) override
    {
        ++pointerEvents;
        return true;
    }

    bool sendWheelEvent(QWheelEvent* event) override
    {
        ++wheelEvents;
        wheelPosition = event->position();
        wheelDelta = event->angleDelta();
        return true;
    }

    bool sendTextInput(const QString& text) override
    {
        ++textEvents;
        committedText.append(text);
        return true;
    }

    int keyEvents = 0;
    int textEvents = 0;
    int pointerEvents = 0;
    int wheelEvents = 0;
    QStringList keyTexts;
    QString committedText;
    QPointF wheelPosition;
    QPoint wheelDelta;
};

SDL_Event keyDown(SDL_Scancode scancode, SDL_Keycode keycode)
{
    SDL_Event event {};
    event.type = SDL_KEYDOWN;
    event.key.type = SDL_KEYDOWN;
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = scancode;
    event.key.keysym.sym = keycode;
    return event;
}

}

class DeckInputDeliveryTest : public QObject
{
    Q_OBJECT

private slots:
    void deliversWheelAndCommitsTextOnlyOnce();
    void sessionOpenSeamAppliesOpeningControllerLayoutFirst();
};

void DeckInputDeliveryTest::deliversWheelAndCommitsTextOnlyOnce()
{
    DeckInputRouter router;
    RecordingSink sink;
    router.openForKeyboard();
    router.setPointerMapping(QRect(100, 50, 800, 450), QSize(1600, 900));

    const auto key = router.route(keyDown(SDL_SCANCODE_E, SDLK_e));
    QCOMPARE(key.action, DeckInputRouter::Action::Key);
    DeckInputDelivery::deliver(key, sink);

    SDL_Event text {};
    text.type = SDL_TEXTINPUT;
    text.text.type = SDL_TEXTINPUT;
    qstrncpy(text.text.text, "é", sizeof(text.text.text));
    DeckInputDelivery::deliver(router.route(text), sink);

    SDL_Event motion {};
    motion.type = SDL_MOUSEMOTION;
    motion.motion.type = SDL_MOUSEMOTION;
    motion.motion.x = 500;
    motion.motion.y = 275;
    DeckInputDelivery::deliver(router.route(motion), sink);

    SDL_Event wheel {};
    wheel.type = SDL_MOUSEWHEEL;
    wheel.wheel.type = SDL_MOUSEWHEEL;
    wheel.wheel.x = 2;
    wheel.wheel.y = -3;
    const auto routedWheel = router.route(wheel);
    QCOMPARE(routedWheel.action, DeckInputRouter::Action::PointerWheel);
    DeckInputDelivery::deliver(routedWheel, sink);

    QCOMPARE(sink.keyEvents, 1);
    QCOMPARE(sink.keyTexts, QStringList({QString()}));
    QCOMPARE(sink.textEvents, 1);
    QCOMPARE(sink.committedText, QString::fromUtf8("é"));
    QCOMPARE(sink.wheelEvents, 1);
    QCOMPARE(sink.wheelPosition, QPointF(800, 450));
    QCOMPARE(sink.wheelDelta, QPoint(240, -360));
}

void DeckInputDeliveryTest::sessionOpenSeamAppliesOpeningControllerLayoutFirst()
{
    OpenHostAdapter adapter;
    ActionRegistry registry({ActionDescriptor {
        QStringLiteral("display.select"),
        QStringLiteral("Select display"),
        ActionCategory::Display,
        {}, {}, {}, 0, ConfirmationPolicy::Never, {},
    }}, adapter);
    DeckController controller(&registry);
    DeckInputRouter::Result opening;
    opening.action = DeckInputRouter::Action::OpenFromController;
    opening.controllerId = 73;
    opening.controllerFamily = ControllerLayout::Family::PlayStation;

    DeckInputDelivery::openDeck(opening, controller, true);

    QVERIFY(controller.isOpen());
    QCOMPARE(controller.controllerLayout()->family(),
             ControllerLayout::Family::PlayStation);
    QVERIFY(controller.controllerLayout()->swapFaceButtons());
    QCOMPARE(controller.controllerLayout()->confirmLabel(),
             QStringLiteral("Circle"));
}

REGISTER_PERIGEE_TEST(DeckInputDeliveryTest);

#include "test_deckinputdelivery.moc"
