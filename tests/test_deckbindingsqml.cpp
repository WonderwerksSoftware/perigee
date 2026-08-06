#include "test_registry.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QAccessible>
#include <QFile>
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
    Q_PROPERTY(int deckPhysicalDisplayCount MEMBER deckPhysicalDisplayCount
               NOTIFY deckPhysicalDisplayCountChanged)

public:
    int deckKeyModifiers = int(Qt::ControlModifier | Qt::AltModifier |
                               Qt::ShiftModifier);
    int deckKeyScancode = SDL_SCANCODE_SPACE;
    int deckControllerButtons = 0x650;
    bool legacyGamepadDisconnect = false;
    int deckPhysicalDisplayCount = 3;
    int logicalKeyboardCaptureCalls = 0;
    int nativeKeyboardCaptureCalls = 0;
    int lastNativeModifiers = 0;
    quint32 lastNativeScanCode = 0;

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
        ++logicalKeyboardCaptureCalls;
        emit deckBindingsChanged();
        return true;
    }

    Q_INVOKABLE bool setDeckKeyboardBindingFromNative(int modifiers,
                                                       quint32 nativeScanCode)
    {
        ++nativeKeyboardCaptureCalls;
        lastNativeModifiers = modifiers;
        lastNativeScanCode = nativeScanCode;
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

    Q_INVOKABLE bool setDeckPhysicalDisplayCount(int count)
    {
        const int normalizedCount = qBound(1, count, 13);
        if (deckPhysicalDisplayCount == normalizedCount) {
            return true;
        }
        deckPhysicalDisplayCount = normalizedCount;
        emit deckPhysicalDisplayCountChanged();
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
    void deckPhysicalDisplayCountChanged();
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
    void keyboardCaptureUsesNativeScanCodeInsteadOfLogicalKey();
    void keypadClassificationModifierIsNotStoredAsPartOfShortcut();
    void keypadClassificationModifierDoesNotSatisfyModifierRequirement();
    void hidingTheSettingsViewCancelsControllerCapture();
    void physicalDisplayCountControlUsesBoundedPreference();
    void focusableSettingsControlsExposeAccessibleDescriptions();
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

void DeckBindingsQmlTest::keyboardCaptureUsesNativeScanCodeInsteadOfLogicalKey()
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
    QVERIFY(QMetaObject::invokeMethod(root.data(), "beginKeyboardCapture"));

    QVERIFY(QMetaObject::invokeMethod(
        root.data(), "handleKeyboardCapture",
        Q_ARG(QVariant, int(Qt::ControlModifier)),
        Q_ARG(QVariant, int(Qt::Key_Z)),
        Q_ARG(QVariant, quint32(29)),
        Q_ARG(QVariant, false)));

    QCOMPARE(preferences.nativeKeyboardCaptureCalls, 1);
    QCOMPARE(preferences.logicalKeyboardCaptureCalls, 0);
    QCOMPARE(preferences.lastNativeScanCode, quint32(29));
    QCOMPARE(root->property("captureMode").toString(), QString());
}

void DeckBindingsQmlTest::keypadClassificationModifierIsNotStoredAsPartOfShortcut()
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
    QVERIFY(QMetaObject::invokeMethod(root.data(), "beginKeyboardCapture"));

    QVERIFY(QMetaObject::invokeMethod(
        root.data(), "handleKeyboardCapture",
        Q_ARG(QVariant, int(Qt::ControlModifier | Qt::KeypadModifier)),
        Q_ARG(QVariant, int(Qt::Key_Enter)),
        Q_ARG(QVariant, quint32(104)),
        Q_ARG(QVariant, false)));

    QCOMPARE(preferences.nativeKeyboardCaptureCalls, 1);
    QCOMPARE(preferences.lastNativeModifiers, int(Qt::ControlModifier));
    QCOMPARE(preferences.lastNativeScanCode, quint32(104));
    QCOMPARE(root->property("captureMode").toString(), QString());
}

void DeckBindingsQmlTest::keypadClassificationModifierDoesNotSatisfyModifierRequirement()
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
    QVERIFY(QMetaObject::invokeMethod(root.data(), "beginKeyboardCapture"));

    QVERIFY(QMetaObject::invokeMethod(
        root.data(), "handleKeyboardCapture",
        Q_ARG(QVariant, int(Qt::KeypadModifier)),
        Q_ARG(QVariant, int(Qt::Key_Enter)),
        Q_ARG(QVariant, quint32(104)),
        Q_ARG(QVariant, false)));

    QCOMPARE(preferences.nativeKeyboardCaptureCalls, 0);
    QCOMPARE(root->property("captureMode").toString(),
             QStringLiteral("keyboard"));
    QVERIFY(!root->property("conflictMessage").toString().isEmpty());
}

void DeckBindingsQmlTest::hidingTheSettingsViewCancelsControllerCapture()
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

    root->setProperty("visible", false);
    QCoreApplication::processEvents();

    QVERIFY(!navigation.captureActive);
    QCOMPARE(root->property("captureMode").toString(), QString());
}

void DeckBindingsQmlTest::physicalDisplayCountControlUsesBoundedPreference()
{
    const QString sourcePath = QFINDTESTDATA(
        "../app/gui/perigee/DeckBindingSettings.qml");
    QVERIFY2(!sourcePath.isEmpty(), "Deck binding settings source not found");
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray source = sourceFile.readAll();

    QVERIFY(source.contains("objectName: \"deckPhysicalDisplayCount\""));
    QVERIFY(source.contains("from: 1"));
    QVERIFY(source.contains("to: 13"));
    QVERIFY(source.contains(
        "preferences.setDeckPhysicalDisplayCount(value)"));
}

void DeckBindingsQmlTest::focusableSettingsControlsExposeAccessibleDescriptions()
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
    QCoreApplication::processEvents();

    const QHash<QString, QString> expectedNames {
        {QStringLiteral("deckKeyboardCaptureButton"),
         QStringLiteral("Capture keyboard shortcut")},
        {QStringLiteral("deckControllerCaptureButton"),
         QStringLiteral("Capture controller chord")},
        {QStringLiteral("deckCaptureCancelButton"),
         QStringLiteral("Cancel binding capture")},
        {QStringLiteral("deckBindingsResetButton"),
         QStringLiteral("Reset Perigee Deck bindings")},
        {QStringLiteral("deckPhysicalDisplayCount"),
         QStringLiteral("Physical displays in Perigee Deck")},
        {QStringLiteral("legacyDisconnectCheck"),
         QStringLiteral("Legacy direct disconnect")},
    };
    for (auto it = expectedNames.cbegin(); it != expectedNames.cend(); ++it) {
        QObject* control = root->findChild<QObject*>(it.key());
        QVERIFY2(control != nullptr, qPrintable(it.key()));
        QAccessibleInterface* interface =
            QAccessible::queryAccessibleInterface(control);
        QVERIFY2(interface != nullptr, qPrintable(it.key()));
        QCOMPARE(interface->text(QAccessible::Name), it.value());
        QVERIFY2(!interface->text(QAccessible::Description).trimmed().isEmpty(),
                 qPrintable(it.key()));
    }
}

REGISTER_PERIGEE_TEST(DeckBindingsQmlTest);

#include "test_deckbindingsqml.moc"
