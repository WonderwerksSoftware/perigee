#include "test_registry.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QtTest>

#include <SDL.h>

class FakeDeckPreferences final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int deckKeyModifiers MEMBER deckKeyModifiers NOTIFY deckBindingsChanged)
    Q_PROPERTY(int deckKeyScancode MEMBER deckKeyScancode NOTIFY deckBindingsChanged)
    Q_PROPERTY(int deckControllerButtons MEMBER deckControllerButtons NOTIFY deckBindingsChanged)
    Q_PROPERTY(bool legacyGamepadDisconnect MEMBER legacyGamepadDisconnect
               NOTIFY legacyGamepadDisconnectChanged)

public:
    int deckKeyModifiers = int(Qt::ControlModifier | Qt::AltModifier |
                               Qt::ShiftModifier);
    int deckKeyScancode = SDL_SCANCODE_SPACE;
    int deckControllerButtons = 0x650;
    bool legacyGamepadDisconnect = false;

    Q_INVOKABLE QString formatDeckKeyboardBinding(int, int) const
    {
        return QStringLiteral("Ctrl+Alt+Shift+Space");
    }

    Q_INVOKABLE QString formatDeckControllerBinding(int) const
    {
        return QStringLiteral("LB+RB+Back+Start");
    }

    Q_INVOKABLE QString deckControllerConflictReason(int buttons) const
    {
        return buttons == 0x614
            ? QStringLiteral("This shortcut is reserved for Moonlight's performance statistics.")
            : QString();
    }

    Q_INVOKABLE bool setDeckKeyboardBindingFromQt(int, int)
    {
        emit deckBindingsChanged();
        return true;
    }

    Q_INVOKABLE bool setDeckControllerBinding(int buttons)
    {
        if (!deckControllerConflictReason(buttons).isEmpty()) {
            return false;
        }
        deckControllerButtons = buttons;
        emit deckBindingsChanged();
        return true;
    }

    Q_INVOKABLE void resetDeckBindings()
    {
        legacyGamepadDisconnect = false;
        emit deckBindingsChanged();
        emit legacyGamepadDisconnectChanged();
    }

signals:
    void deckBindingsChanged();
    void legacyGamepadDisconnectChanged();
};

class FakeGamepadNavigation final : public QObject
{
    Q_OBJECT

public:
    bool captureActive = false;

    Q_INVOKABLE void beginControllerBindingCapture()
    {
        captureActive = true;
    }

    Q_INVOKABLE void cancelControllerBindingCapture()
    {
        captureActive = false;
    }

signals:
    void controllerBindingCaptured(int buttons);
    void controllerBindingCaptureCancelled();
};

class DeckBindingsQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void captureCanBeCancelledExplicitly();
    void controllerBackCancelsCapture();
    void statsConflictStaysInCaptureAndExplainsTheProblem();
};

void DeckBindingsQmlTest::captureCanBeCancelledExplicitly()
{
    QQmlEngine engine;
    QQmlComponent component(&engine,
        QUrl(QStringLiteral("qrc:/gui/perigee/DeckBindingSettings.qml")));
    FakeDeckPreferences preferences;
    FakeGamepadNavigation navigation;
    QScopedPointer<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("gamepadNavigation"), QVariant::fromValue(&navigation)},
    }));
    QVERIFY2(root, qPrintable(component.errorString()));

    QVERIFY(QMetaObject::invokeMethod(root.data(), "beginControllerCapture"));
    QVERIFY(navigation.captureActive);
    QCOMPARE(root->property("captureMode").toString(), QStringLiteral("controller"));

    QVERIFY(QMetaObject::invokeMethod(root.data(), "cancelCapture"));
    QVERIFY(!navigation.captureActive);
    QCOMPARE(root->property("captureMode").toString(), QString());
    QCOMPARE(root->property("conflictMessage").toString(), QString());
}

void DeckBindingsQmlTest::controllerBackCancelsCapture()
{
    QQmlEngine engine;
    QQmlComponent component(&engine,
        QUrl(QStringLiteral("qrc:/gui/perigee/DeckBindingSettings.qml")));
    FakeDeckPreferences preferences;
    FakeGamepadNavigation navigation;
    QScopedPointer<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("gamepadNavigation"), QVariant::fromValue(&navigation)},
    }));
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(QMetaObject::invokeMethod(root.data(), "beginControllerCapture"));

    emit navigation.controllerBindingCaptureCancelled();
    QCoreApplication::processEvents();

    QCOMPARE(root->property("captureMode").toString(), QString());
    QCOMPARE(root->property("conflictMessage").toString(), QString());
}

void DeckBindingsQmlTest::statsConflictStaysInCaptureAndExplainsTheProblem()
{
    QQmlEngine engine;
    QQmlComponent component(&engine,
        QUrl(QStringLiteral("qrc:/gui/perigee/DeckBindingSettings.qml")));
    FakeDeckPreferences preferences;
    FakeGamepadNavigation navigation;
    QScopedPointer<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
        {QStringLiteral("gamepadNavigation"), QVariant::fromValue(&navigation)},
    }));
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(QMetaObject::invokeMethod(root.data(), "beginControllerCapture"));

    emit navigation.controllerBindingCaptured(0x614);
    QCoreApplication::processEvents();

    QCOMPARE(root->property("captureMode").toString(), QStringLiteral("controller"));
    QCOMPARE(root->property("conflictMessage").toString(),
             QStringLiteral("This shortcut is reserved for Moonlight's performance statistics."));
}

REGISTER_PERIGEE_TEST(DeckBindingsQmlTest);

#include "test_deckbindingsqml.moc"
