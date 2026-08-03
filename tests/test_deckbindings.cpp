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
    void controllerBindingsRejectReservedPrefixRelationships();
    void controllerBindingsRejectImpossibleDirections();
    void extendedFunctionKeysMapToTheirPhysicalScancodes();
    void waylandNativeScanCodesMapPhysicalKeysWithoutLogicalTranslation();
    void resetRestoresDefaultsAndDisablesLegacyMode();
    void controllerCaptureCompletesOnlyAfterEveryButtonIsReleased();
    void controllerCaptureUsesTheLargestSimultaneousChordNotRolledUnion();
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
        SDL_CONTROLLER_BUTTON_START,
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

void DeckBindingsTest::controllerBindingsRejectReservedPrefixRelationships()
{
    const quint32 lb = quint32(1) << SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    const quint32 rb = quint32(1) << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    const quint32 back = quint32(1) << SDL_CONTROLLER_BUTTON_BACK;
    const quint32 start = quint32(1) << SDL_CONTROLLER_BUTTON_START;
    const quint32 a = quint32(1) << SDL_CONTROLLER_BUTTON_A;
    const quint32 x = quint32(1) << SDL_CONTROLLER_BUTTON_X;
    const quint32 y = quint32(1) << SDL_CONTROLLER_BUTTON_Y;
    const quint32 statsPrefix = lb | rb | back;

    const QList<quint32> conflicts {
        lb | rb,
        statsPrefix | x,
        statsPrefix | x | a,
        statsPrefix | y,
        statsPrefix | y | a,
        lb | rb | back | start | a,
    };
    for (quint32 conflict : conflicts) {
        QVERIFY2(!DeckBindings::isValidControllerBinding(conflict),
                 qPrintable(QStringLiteral("unexpectedly accepted 0x%1")
                                .arg(conflict, 0, 16)));
        QVERIFY(!DeckBindings::controllerConflictReason(conflict).isEmpty());
    }

    QVERIFY(DeckBindings::isValidControllerBinding(lb | rb | back | start));
}

void DeckBindingsTest::controllerBindingsRejectImpossibleDirections()
{
    const quint32 impossible =
        (quint32(1) << SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_DPAD_UP) |
        (quint32(1) << SDL_CONTROLLER_BUTTON_DPAD_DOWN);

    QVERIFY(!DeckBindings::isValidControllerBinding(impossible));
    QVERIFY(DeckBindings::controllerConflictReason(impossible)
                .contains(QStringLiteral("opposite"), Qt::CaseInsensitive));
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

void DeckBindingsTest::waylandNativeScanCodesMapPhysicalKeysWithoutLogicalTranslation()
{
    // Wayland/Qt uses XKB keycodes: Linux evdev code plus the XKB offset 8.
    QCOMPARE(DeckBindings::sdlScancodeForNativeKey(29, QStringLiteral("wayland")),
             int(SDL_SCANCODE_Y));
    QCOMPARE(DeckBindings::sdlScancodeForNativeKey(38, QStringLiteral("wayland-egl")),
             int(SDL_SCANCODE_A));
    QCOMPARE(DeckBindings::sdlScancodeForNativeKey(104, QStringLiteral("wayland")),
             int(SDL_SCANCODE_KP_ENTER));
    QCOMPARE(DeckBindings::sdlScancodeForNativeKey(29, QStringLiteral("xcb")),
             int(SDL_SCANCODE_UNKNOWN));
    QCOMPARE(DeckBindings::sdlScancodeForNativeKey(29, QStringLiteral("offscreen")),
             int(SDL_SCANCODE_UNKNOWN));
    QCOMPARE(DeckBindings::sdlScancodeForNativeKey(0, QStringLiteral("wayland")),
             int(SDL_SCANCODE_UNKNOWN));
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

void DeckBindingsTest::controllerCaptureUsesTheLargestSimultaneousChordNotRolledUnion()
{
    DeckControllerChordCapture capture;
    capture.begin();

    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true));
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_DPAD_UP, true));
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_DPAD_UP, false));
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true));
    QVERIFY(!capture.handleButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false));
    const std::optional<quint32> completed =
        capture.handleButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);

    QVERIFY(completed.has_value());
    QCOMPARE(*completed,
             buttonMask({SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                         SDL_CONTROLLER_BUTTON_DPAD_UP}));
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
