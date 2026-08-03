#include "test_registry.h"

#define private public
#include "settings/streamingpreferences.h"
#undef private

#include "perigee/input/deckbindings.h"

#include <QCoreApplication>
#include <QMetaProperty>
#include <QSettings>
#include <QSignalSpy>
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

}

class StreamingPreferencesTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void actualPropertiesExposeAndEmitTheirNotifySignals();
    void actualSaveAndReloadRoundTripValidatedDeckBindings();
    void unsupportedPlatformRejectsNativeCaptureWithoutChangingBinding();

private:
    QTemporaryDir m_SettingsDirectory;
};

void StreamingPreferencesTest::initTestCase()
{
    QVERIFY(m_SettingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("PerigeeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("StreamingPreferences"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_SettingsDirectory.path());
}

void StreamingPreferencesTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
}

void StreamingPreferencesTest::actualPropertiesExposeAndEmitTheirNotifySignals()
{
    StreamingPreferences preferences(nullptr);
    const QMetaObject* meta = preferences.metaObject();
    const struct ExpectedProperty {
        const char* name;
        const char* notify;
    } expected[] {
        {"deckKeyModifiers", "deckBindingsChanged"},
        {"deckKeyScancode", "deckBindingsChanged"},
        {"deckControllerButtons", "deckBindingsChanged"},
        {"legacyGamepadDisconnect", "legacyGamepadDisconnectChanged"},
    };
    for (const ExpectedProperty& property : expected) {
        const int index = meta->indexOfProperty(property.name);
        QVERIFY(index >= 0);
        const QMetaProperty metaProperty = meta->property(index);
        QVERIFY(metaProperty.hasNotifySignal());
        QCOMPARE(metaProperty.notifySignal().name(), QByteArray(property.notify));
    }

    QSignalSpy bindingChanged(&preferences,
                              &StreamingPreferences::deckBindingsChanged);
    const quint32 controller = buttonMask({
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
    });
    QVERIFY(preferences.setDeckControllerBinding(int(controller)));
    QCOMPARE(bindingChanged.size(), 1);

    QSignalSpy legacyChanged(
        &preferences,
        &StreamingPreferences::legacyGamepadDisconnectChanged);
    QVERIFY(preferences.setProperty("legacyGamepadDisconnect", true));
    QCOMPARE(legacyChanged.size(), 1);
}

void StreamingPreferencesTest::actualSaveAndReloadRoundTripValidatedDeckBindings()
{
    const quint32 controller = buttonMask({
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
    });
    StreamingPreferences saved(nullptr);
    saved.deckKeyModifiers = int(Qt::ControlModifier | Qt::MetaModifier);
    saved.deckKeyScancode = SDL_SCANCODE_F8;
    saved.deckControllerButtons = int(controller);
    saved.legacyGamepadDisconnect = true;
    saved.save();

    StreamingPreferences reloaded(nullptr);
    QCOMPARE(reloaded.deckKeyModifiers,
             int(Qt::ControlModifier | Qt::MetaModifier));
    QCOMPARE(reloaded.deckKeyScancode, int(SDL_SCANCODE_F8));
    QCOMPARE(reloaded.deckControllerButtons, int(controller));
    QVERIFY(reloaded.legacyGamepadDisconnect);
}

void StreamingPreferencesTest::unsupportedPlatformRejectsNativeCaptureWithoutChangingBinding()
{
    StreamingPreferences preferences(nullptr);
    const int originalModifiers = preferences.deckKeyModifiers;
    const int originalScancode = preferences.deckKeyScancode;

    QVERIFY(!preferences.setDeckKeyboardBindingFromNative(
        int(Qt::ControlModifier), 29));

    QCOMPARE(preferences.deckKeyModifiers, originalModifiers);
    QCOMPARE(preferences.deckKeyScancode, originalScancode);
}

REGISTER_PERIGEE_TEST(StreamingPreferencesTest);

#include "test_streamingpreferences.moc"
