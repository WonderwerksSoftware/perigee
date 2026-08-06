#include "test_registry.h"

#define private public
#include "settings/streamingpreferences.h"
#undef private

#include "perigee/input/deckbindings.h"

#include <QCoreApplication>
#include <QMetaProperty>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString g_OriginalOrganizationName;
QString g_OriginalApplicationName;
QSettings::Format g_OriginalSettingsFormat = QSettings::NativeFormat;
QByteArray g_OriginalConfigHome;
bool g_HadOriginalConfigHome = false;
bool g_StreamingPreferencesGlobalsRestored = false;

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
    void cleanupTestCase();
    void init();
    void actualPropertiesExposeAndEmitTheirNotifySignals();
    void actualSaveAndReloadRoundTripValidatedDeckBindings();
    void physicalDisplayCountClampsAndPersists();
    void unsupportedPlatformRejectsNativeCaptureWithoutChangingBinding();

private:
    QTemporaryDir m_SettingsDirectory;
};

void StreamingPreferencesTest::initTestCase()
{
    QVERIFY(m_SettingsDirectory.isValid());
    g_OriginalOrganizationName = QCoreApplication::organizationName();
    g_OriginalApplicationName = QCoreApplication::applicationName();
    g_OriginalSettingsFormat = QSettings::defaultFormat();
    g_HadOriginalConfigHome = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    g_OriginalConfigHome = qgetenv("XDG_CONFIG_HOME");

    if (qEnvironmentVariableIsEmpty(
            "PERIGEE_STREAMING_PREFERENCES_CHILD")) {
        QVERIFY(qputenv("XDG_CONFIG_HOME",
                        m_SettingsDirectory.path().toUtf8()));
    }
    QCoreApplication::setOrganizationName(QStringLiteral("PerigeeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("StreamingPreferences"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
}

void StreamingPreferencesTest::cleanupTestCase()
{
    QCoreApplication::setOrganizationName(g_OriginalOrganizationName);
    QCoreApplication::setApplicationName(g_OriginalApplicationName);
    QSettings::setDefaultFormat(g_OriginalSettingsFormat);
    if (g_HadOriginalConfigHome) {
        QVERIFY(qputenv("XDG_CONFIG_HOME", g_OriginalConfigHome));
    }
    else {
        QVERIFY(qunsetenv("XDG_CONFIG_HOME"));
    }
    g_StreamingPreferencesGlobalsRestored = true;
}

void StreamingPreferencesTest::init()
{
    if (qEnvironmentVariableIsEmpty(
            "PERIGEE_STREAMING_PREFERENCES_CHILD")) {
        return;
    }
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
        {"deckPhysicalDisplayCount", "deckPhysicalDisplayCountChanged"},
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

    QSignalSpy displayCountChanged(
        &preferences, SIGNAL(deckPhysicalDisplayCountChanged()));
    QVERIFY(displayCountChanged.isValid());
    QVERIFY(preferences.setProperty("deckPhysicalDisplayCount", 14));
    QCOMPARE(preferences.property("deckPhysicalDisplayCount").toInt(), 13);
    QCOMPARE(displayCountChanged.size(), 1);
    QVERIFY(preferences.setProperty("deckPhysicalDisplayCount", 13));
    QCOMPARE(displayCountChanged.size(), 1);
}

void StreamingPreferencesTest::actualSaveAndReloadRoundTripValidatedDeckBindings()
{
    if (qEnvironmentVariableIsEmpty(
            "PERIGEE_STREAMING_PREFERENCES_CHILD")) {
        QProcess child;
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QStringLiteral("PERIGEE_STREAMING_PREFERENCES_CHILD"),
            QStringLiteral("1"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                           m_SettingsDirectory.path());
        child.setProcessEnvironment(environment);
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral("StreamingPreferencesTest"),
            QStringLiteral(
                "actualSaveAndReloadRoundTripValidatedDeckBindings"),
        });
        child.start();
        QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(30000),
                 qPrintable(child.errorString()));
        const QByteArray output = child.readAllStandardOutput() +
            child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }

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

void StreamingPreferencesTest::physicalDisplayCountClampsAndPersists()
{
    if (qEnvironmentVariableIsEmpty(
            "PERIGEE_STREAMING_PREFERENCES_CHILD")) {
        QProcess child;
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            QStringLiteral("PERIGEE_STREAMING_PREFERENCES_CHILD"),
            QStringLiteral("1"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                           m_SettingsDirectory.path());
        child.setProcessEnvironment(environment);
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({
            QStringLiteral("StreamingPreferencesTest"),
            QStringLiteral("physicalDisplayCountClampsAndPersists"),
        });
        child.start();
        QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(30000),
                 qPrintable(child.errorString()));
        const QByteArray output = child.readAllStandardOutput() +
            child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }

    const struct TestCase {
        int requested;
        int expected;
    } cases[] {
        {0, 1},
        {3, 3},
        {14, 13},
    };

    for (const TestCase& testCase : cases) {
        QSettings settings;
        settings.clear();
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);

        StreamingPreferences saved(nullptr);
        QVERIFY(saved.setProperty("deckPhysicalDisplayCount",
                                  testCase.requested));
        QCOMPARE(saved.property("deckPhysicalDisplayCount").toInt(),
                 testCase.expected);
        saved.save();

        StreamingPreferences reloaded(nullptr);
        QCOMPARE(reloaded.property("deckPhysicalDisplayCount").toInt(),
                 testCase.expected);
    }
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

class StreamingPreferencesIsolationTest : public QObject
{
    Q_OBJECT

private slots:
    void priorPreferenceTestsRestoredProcessGlobals();
};

void StreamingPreferencesIsolationTest::priorPreferenceTestsRestoredProcessGlobals()
{
    if (!g_StreamingPreferencesGlobalsRestored) {
        QSKIP("This sentinel verifies ordering in the monolithic test run.");
    }
    QCOMPARE(QCoreApplication::organizationName(),
             g_OriginalOrganizationName);
    QCOMPARE(QCoreApplication::applicationName(),
             g_OriginalApplicationName);
    QCOMPARE(QSettings::defaultFormat(), g_OriginalSettingsFormat);
    QCOMPARE(qEnvironmentVariableIsSet("XDG_CONFIG_HOME"),
             g_HadOriginalConfigHome);
    QCOMPARE(qgetenv("XDG_CONFIG_HOME"), g_OriginalConfigHome);
}

REGISTER_PERIGEE_TEST(StreamingPreferencesIsolationTest);

#include "test_streamingpreferences.moc"
