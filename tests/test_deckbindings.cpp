#include "test_registry.h"

#include "perigee/input/deckbindings.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

quint32 buttonMask(std::initializer_list<SDL_GameControllerButton> buttons)
{
    quint32 mask = 0;
    for (SDL_GameControllerButton button : buttons) {
        mask |= quint32(1) << button;
    }
    return mask;
}

QSettings isolatedSettings(const QTemporaryDir& directory)
{
    return QSettings(directory.filePath(QStringLiteral("perigee.ini")),
                     QSettings::IniFormat);
}

}

class DeckBindingsTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreSafePhysicalChords();
    void validBindingsRoundTripThroughIsolatedSettings();
    void invalidKeyboardStorageFallsBackAsOneBinding();
    void invalidControllerStorageFallsBackWithoutShadowingStats();
    void extendedFunctionKeysMapToTheirPhysicalScancodes();
    void resetRestoresDefaultsAndDisablesLegacyMode();
    void controllerCaptureCompletesOnlyAfterEveryButtonIsReleased();
    void controllerCaptureCanBeCancelledWithoutAResult();
};

void DeckBindingsTest::defaultsAreSafePhysicalChords()
{
    const DeckBindings bindings;

    QCOMPARE(bindings.keyModifiers(),
             int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
    QCOMPARE(bindings.keyScancode(), int(SDL_SCANCODE_SPACE));
    QCOMPARE(bindings.controllerButtons(), buttonMask({
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
    }));
    QVERIFY(!bindings.legacyGamepadDisconnect());
}

void DeckBindingsTest::validBindingsRoundTripThroughIsolatedSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = isolatedSettings(directory);
    const quint32 controller = buttonMask({
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_Y,
    });
    const DeckBindings saved(int(Qt::ControlModifier | Qt::MetaModifier),
                             SDL_SCANCODE_F8,
                             controller,
                             true);

    saved.save(settings);
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    QSettings reloadedSettings = isolatedSettings(directory);
    const DeckBindings reloaded = DeckBindings::load(reloadedSettings);
    QCOMPARE(reloaded.keyModifiers(),
             int(Qt::ControlModifier | Qt::MetaModifier));
    QCOMPARE(reloaded.keyScancode(), int(SDL_SCANCODE_F8));
    QCOMPARE(reloaded.controllerButtons(), controller);
    QVERIFY(reloaded.legacyGamepadDisconnect());
}

void DeckBindingsTest::invalidKeyboardStorageFallsBackAsOneBinding()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings = isolatedSettings(directory);

    settings.setValue(QStringLiteral("deckKeyModifiers"), 0);
    settings.setValue(QStringLiteral("deckKeyScancode"), SDL_SCANCODE_F8);
    DeckBindings emptyModifiers = DeckBindings::load(settings);
    QCOMPARE(emptyModifiers.keyModifiers(),
             int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
    QCOMPARE(emptyModifiers.keyScancode(), int(SDL_SCANCODE_SPACE));

    settings.setValue(QStringLiteral("deckKeyModifiers"),
                      int(Qt::ControlModifier));
    settings.setValue(QStringLiteral("deckKeyScancode"),
                      int(SDL_SCANCODE_LCTRL));
    DeckBindings modifierOnlyKey = DeckBindings::load(settings);
    QCOMPARE(modifierOnlyKey.keyModifiers(),
             int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
    QCOMPARE(modifierOnlyKey.keyScancode(), int(SDL_SCANCODE_SPACE));

    settings.setValue(QStringLiteral("deckKeyModifiers"),
                      int(Qt::ControlModifier) | 0x0200);
    settings.setValue(QStringLiteral("deckKeyScancode"), int(SDL_SCANCODE_F8));
    DeckBindings unsupportedModifier = DeckBindings::load(settings);
    QCOMPARE(unsupportedModifier.keyModifiers(),
             int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
    QCOMPARE(unsupportedModifier.keyScancode(), int(SDL_SCANCODE_SPACE));
}

void DeckBindingsTest::invalidControllerStorageFallsBackWithoutShadowingStats()
{
    const quint32 defaultChord = buttonMask({
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
    });
    const QList<quint32> invalidMasks {
        0,
        quint32(1) << SDL_CONTROLLER_BUTTON_A,
        buttonMask({SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                    SDL_CONTROLLER_BUTTON_BACK,
                    SDL_CONTROLLER_BUTTON_X}),
        quint32(1) << 31,
    };

    for (quint32 invalidMask : invalidMasks) {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings = isolatedSettings(directory);
        settings.setValue(QStringLiteral("deckControllerButtons"), invalidMask);

        const DeckBindings bindings = DeckBindings::load(settings);
        QCOMPARE(bindings.controllerButtons(), defaultChord);
    }
}

void DeckBindingsTest::extendedFunctionKeysMapToTheirPhysicalScancodes()
{
    QCOMPARE(DeckBindings::sdlScancodeForQtKey(Qt::Key_F12),
             int(SDL_SCANCODE_F12));
    QCOMPARE(DeckBindings::sdlScancodeForQtKey(Qt::Key_F13),
             int(SDL_SCANCODE_F13));
    QCOMPARE(DeckBindings::sdlScancodeForQtKey(Qt::Key_F24),
             int(SDL_SCANCODE_F24));
}

void DeckBindingsTest::resetRestoresDefaultsAndDisablesLegacyMode()
{
    DeckBindings bindings(int(Qt::ControlModifier | Qt::MetaModifier),
                          SDL_SCANCODE_F8,
                          buttonMask({SDL_CONTROLLER_BUTTON_BACK,
                                      SDL_CONTROLLER_BUTTON_Y}),
                          true);

    bindings.resetToDefaults();

    QCOMPARE(bindings.keyModifiers(),
             int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
    QCOMPARE(bindings.keyScancode(), int(SDL_SCANCODE_SPACE));
    QCOMPARE(bindings.controllerButtons(), buttonMask({
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
    }));
    QVERIFY(!bindings.legacyGamepadDisconnect());
}

void DeckBindingsTest::controllerCaptureCompletesOnlyAfterEveryButtonIsReleased()
{
    DeckControllerChordCapture capture;
    capture.begin();

    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true));
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_BACK, true));
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false));
    const std::optional<quint32> completed =
        capture.handleButton(SDL_CONTROLLER_BUTTON_BACK, false);

    QVERIFY(completed.has_value());
    QCOMPARE(*completed,
             buttonMask({SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                         SDL_CONTROLLER_BUTTON_BACK}));
    QVERIFY(!capture.isActive());
}

void DeckBindingsTest::controllerCaptureCanBeCancelledWithoutAResult()
{
    DeckControllerChordCapture capture;
    capture.begin();
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_A, true));

    capture.cancel();

    QVERIFY(!capture.isActive());
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_A, false));
}

REGISTER_PERIGEE_TEST(DeckBindingsTest);

#include "test_deckbindings.moc"
