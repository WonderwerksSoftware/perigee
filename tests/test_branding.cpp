#include "test_registry.h"
#include "perigee/branding/productidentity.h"
#include "settings/moonlightsettingsimport.h"

#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

namespace {

struct StartupGateCounts {
    int computerManagerCreations = 0;
    int preferenceCreations = 0;
    int navigationCreations = 0;
    int pollingStarts = 0;
    int focusNotifications = 0;
};

StartupGateCounts startupGateCounts;

QList<bool> settingsWriteFailures;

bool readFailedSyncSettings(QIODevice& device,
                            QSettings::SettingsMap& settings)
{
    if (device.size() == 0) {
        settings.clear();
        return true;
    }

    QDataStream stream(&device);
    stream >> settings;
    return stream.status() == QDataStream::Ok;
}

bool writeFailedSyncSettings(QIODevice& device,
                             const QSettings::SettingsMap& settings)
{
    QDataStream stream(&device);
    const bool shouldFail = !settingsWriteFailures.isEmpty()
        && settingsWriteFailures.takeFirst();
    if (shouldFail) {
        QSettings::SettingsMap partialSettings;
        if (!settings.isEmpty()) {
            partialSettings.insert(settings.cbegin().key(),
                                   settings.cbegin().value());
        }
        stream << partialSettings;
        return false;
    }

    stream << settings;
    return stream.status() == QDataStream::Ok;
}

QSettings::Format failedSyncSettingsFormat()
{
    static const QSettings::Format format = QSettings::registerFormat(
        QStringLiteral("perigee-failed-sync"),
        readFailedSyncSettings,
        writeFailedSyncSettings,
        Qt::CaseSensitive);
    return format;
}

QSettings::SettingsMap readFailedSyncSettingsFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QSettings::SettingsMap settings;
    QDataStream stream(&file);
    stream >> settings;
    return stream.status() == QDataStream::Ok
        ? settings
        : QSettings::SettingsMap {};
}

class StartupGateComputerManager final : public QObject
{
    Q_OBJECT

public:
    explicit StartupGateComputerManager(QObject* parent = nullptr)
        : QObject(parent)
    {
        ++startupGateCounts.computerManagerCreations;
    }

    Q_INVOKABLE void startPolling()
    {
        ++startupGateCounts.pollingStarts;
    }

    Q_INVOKABLE void stopPollingAsync()
    {
    }
};

class StartupGatePreferences final : public QObject
{
    Q_OBJECT

public:
    explicit StartupGatePreferences(QObject* parent = nullptr)
        : QObject(parent)
    {
        ++startupGateCounts.preferenceCreations;
    }

    Q_INVOKABLE void retranslate()
    {
    }
};

class StartupGateNavigation final : public QObject
{
    Q_OBJECT

public:
    explicit StartupGateNavigation(QObject* parent = nullptr)
        : QObject(parent)
    {
        ++startupGateCounts.navigationCreations;
    }

    Q_INVOKABLE void enable()
    {
    }

    Q_INVOKABLE void notifyWindowFocus(bool)
    {
        ++startupGateCounts.focusNotifications;
    }
};

class StartupGateSystemProperties final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasBrowser READ falseValue CONSTANT)
    Q_PROPERTY(QString versionString READ emptyString CONSTANT)

public:
    bool falseValue() const { return false; }
    QString emptyString() const { return {}; }
};

QObject* createStartupGateComputerManager(QQmlEngine*, QJSEngine*)
{
    return new StartupGateComputerManager;
}

QObject* createStartupGatePreferences(QQmlEngine*, QJSEngine*)
{
    return new StartupGatePreferences;
}

QObject* createStartupGateNavigation(QQmlEngine*, QJSEngine*)
{
    return new StartupGateNavigation;
}

QObject* createStartupGateSystemProperties(QQmlEngine*, QJSEngine*)
{
    return new StartupGateSystemProperties;
}

QString sourceRoot()
{
    QDir directory(QCoreApplication::applicationDirPath());
    if (!directory.cdUp() || !directory.cdUp()) {
        return {};
    }
    return directory.canonicalPath();
}

QString applicationPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../app/perigee"));
}

QMap<QString, QString> appStreamFields(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QMap<QString, QString> fields;
    QXmlStreamReader xml(&file);
    QStringList elementPath;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            elementPath.append(xml.name().toString());
            const QString joinedPath = elementPath.join(QLatin1Char('/'));
            if (joinedPath == QStringLiteral("component/releases/release")
                    && !fields.contains(QStringLiteral("release"))) {
                fields.insert(QStringLiteral("release"),
                              xml.attributes().value(QStringLiteral("version")).toString());
            }
            else if (joinedPath == QStringLiteral("component/launchable")) {
                fields.insert(QStringLiteral("launchable-type"),
                              xml.attributes().value(QStringLiteral("type")).toString());
            }
        }
        else if (xml.isCharacters() && !xml.isWhitespace()) {
            const QString value = xml.text().toString().trimmed();
            const QString joinedPath = elementPath.join(QLatin1Char('/'));
            static const QMap<QString, QString> expectedPaths {
                {QStringLiteral("component/id"), QStringLiteral("id")},
                {QStringLiteral("component/launchable"), QStringLiteral("launchable")},
                {QStringLiteral("component/provides/binary"), QStringLiteral("binary")},
                {QStringLiteral("component/name"), QStringLiteral("name")},
                {QStringLiteral("component/summary"), QStringLiteral("summary")},
            };
            if (expectedPaths.contains(joinedPath)) {
                fields.insert(expectedPaths.value(joinedPath), value);
            }
        }
        else if (xml.isEndElement() && !elementPath.isEmpty()) {
            elementPath.removeLast();
        }
    }

    if (xml.hasError()) {
        return {};
    }
    return fields;
}

}

class BrandingTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void runtimeReportsPerigeeVersion();
    void runtimeHelpUsesPerigeeCommand();
    void desktopMetadataUsesPerigeeIdentity();
    void appStreamMetadataUsesPerigeeIdentity();
    void referencedBrandResourceExists();
    void applicationIdentityUsesPerigeeNamespace();
    void productionBinaryHasNoMoonlightUpdateChannel();
    void crossPlatformPackagingUsesPerigeeTargets();
    void crossPlatformPackagingUsesPerigeeIcons();
    void allArtifactConsumersUsePerigeeNames();
    void iconGeneratorFailsWhenSourceIsMissing();
    void githubSupportSurfaceUsesPerigeeIdentity();
    void declinedImportMovesNoProfileData();
    void consentImportsOnlyRecognizedDataAndPairingIdentity();
    void handledDecisionIsIdempotent();
    void importDoesNotOverwriteInitializedPerigeeProfile();
    void pairingIdentityRequiresAnImportablePairedHost();
    void incompletePairingIdentityIsNotImportable();
    void interruptedImportKeepsDurableRecoveryMarkerAcrossConsecutiveFailures();
    void coldReopenBlocksPartialProfileUntilRecoveryCanPersist();
    void importPromptDoesNotInitializeStreamingState();
};

void BrandingTest::initTestCase()
{
    bool failureCountValid = false;
    const int failureCount = qEnvironmentVariableIntValue(
        "PERIGEE_SETTINGS_WRITE_FAILURES", &failureCountValid);
    if (failureCountValid && failureCount > 0) {
        settingsWriteFailures.fill(true, failureCount);
    }

    if (!qEnvironmentVariableIsSet("PERIGEE_STARTUP_GATE_PROBE")) {
        return;
    }

    qmlRegisterSingletonType<StartupGateComputerManager>(
        "ComputerManager", 1, 0, "ComputerManager",
        createStartupGateComputerManager);
    qmlRegisterSingletonType<StartupGatePreferences>(
        "StreamingPreferences", 1, 0, "StreamingPreferences",
        createStartupGatePreferences);
    qmlRegisterSingletonType<StartupGateNavigation>(
        "SdlGamepadKeyNavigation", 1, 0, "SdlGamepadKeyNavigation",
        createStartupGateNavigation);
    qmlRegisterSingletonType<StartupGateSystemProperties>(
        "SystemProperties", 1, 0, "SystemProperties",
        createStartupGateSystemProperties);
}

void BrandingTest::runtimeReportsPerigeeVersion()
{
    const QString binary = applicationPath();
    QVERIFY2(QFileInfo::exists(binary), qPrintable(binary));

    QProcess process;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), profile.path());
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), profile.path());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QML_DISABLE_DISK_CACHE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("MESA_SHADER_CACHE_DISABLE"), QStringLiteral("true"));
    environment.insert(QStringLiteral("SDL_VIDEODRIVER"), QStringLiteral("dummy"));
    process.setProcessEnvironment(environment);
    process.start(binary, {QStringLiteral("--version")});
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(process.readAll()).trimmed(),
             QStringLiteral("Perigee 0.1.0"));
    QDir profileDirectory(profile.path());
    QCOMPARE(profileDirectory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList());
}

void BrandingTest::runtimeHelpUsesPerigeeCommand()
{
    QProcess process;
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), profile.path());
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), profile.path());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QML_DISABLE_DISK_CACHE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("MESA_SHADER_CACHE_DISABLE"), QStringLiteral("true"));
    environment.insert(QStringLiteral("SDL_VIDEODRIVER"), QStringLiteral("dummy"));
    process.setProcessEnvironment(environment);
    process.start(applicationPath(), {QStringLiteral("--help")});
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    const QString output = QString::fromUtf8(process.readAll());
    QVERIFY(output.contains(QStringLiteral("Starts Perigee normally")));
    QVERIFY(output.contains(QStringLiteral("perigee <action> --help")));
    QVERIFY(!output.contains(QStringLiteral("moonlight <action> --help")));
    QDir profileDirectory(profile.path());
    QCOMPARE(profileDirectory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList());
}

void BrandingTest::desktopMetadataUsesPerigeeIdentity()
{
    const QString path = QDir(sourceRoot()).filePath(
        QStringLiteral("app/deploy/linux/app.perigee_stream.Perigee.desktop"));
    QVERIFY2(QFileInfo::exists(path), qPrintable(path));

    QSettings desktop(path, QSettings::IniFormat);
    desktop.beginGroup(QStringLiteral("Desktop Entry"));
    QCOMPARE(desktop.value(QStringLiteral("Name")).toString(),
             QStringLiteral("Perigee"));
    QCOMPARE(desktop.value(QStringLiteral("Exec")).toString(),
             QStringLiteral("perigee"));
    QCOMPARE(desktop.value(QStringLiteral("Icon")).toString(),
             QStringLiteral("perigee"));
}

void BrandingTest::appStreamMetadataUsesPerigeeIdentity()
{
    const QString path = QDir(sourceRoot()).filePath(
        QStringLiteral("app/deploy/linux/app.perigee_stream.Perigee.appdata.xml"));
    const QMap<QString, QString> fields = appStreamFields(path);
    QVERIFY2(!fields.isEmpty(), qPrintable(path));
    QCOMPARE(fields.value(QStringLiteral("id")),
             QStringLiteral("app.perigee_stream.Perigee"));
    QCOMPARE(fields.value(QStringLiteral("launchable")),
             QStringLiteral("app.perigee_stream.Perigee.desktop"));
    QCOMPARE(fields.value(QStringLiteral("launchable-type")),
             QStringLiteral("desktop-id"));
    QCOMPARE(fields.value(QStringLiteral("binary")),
             QStringLiteral("perigee"));
    QCOMPARE(fields.value(QStringLiteral("name")),
             QStringLiteral("Perigee"));
    QCOMPARE(fields.value(QStringLiteral("release")),
             QStringLiteral("0.1.0"));
    QVERIFY(fields.value(QStringLiteral("summary")).contains(
        QStringLiteral("remote"), Qt::CaseInsensitive));
}

void BrandingTest::referencedBrandResourceExists()
{
    QFile resource(QStringLiteral(":/res/perigee.svg"));
    QVERIFY2(resource.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(resource.fileName()));
    const QByteArray contents = resource.readAll();
    QVERIFY(contents.contains("<svg"));
}

void BrandingTest::applicationIdentityUsesPerigeeNamespace()
{
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldDomain = QCoreApplication::organizationDomain();
    const QString oldApplication = QCoreApplication::applicationName();

    ProductIdentity::apply();
    QCOMPARE(QCoreApplication::organizationName(),
             QStringLiteral("Perigee Streaming Project"));
    QCOMPARE(QCoreApplication::organizationDomain(),
             QStringLiteral("perigee-stream.app"));
    QCOMPARE(QCoreApplication::applicationName(), QStringLiteral("Perigee"));
    QCOMPARE(ProductIdentity::windowTitle(), QStringLiteral("Perigee"));

    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setOrganizationDomain(oldDomain);
    QCoreApplication::setApplicationName(oldApplication);
}

void BrandingTest::productionBinaryHasNoMoonlightUpdateChannel()
{
    QFile binary(applicationPath());
    QVERIFY2(binary.open(QIODevice::ReadOnly), qPrintable(binary.fileName()));
    QVERIFY(!binary.readAll().contains(
        "https://moonlight-stream.org/updates/qt.json"));
}

void BrandingTest::crossPlatformPackagingUsesPerigeeTargets()
{
    const QStringList packagingFiles {
        QStringLiteral("app/app.pro"),
        QStringLiteral("app/Info.plist"),
        QStringLiteral("scripts/build-arch.bat"),
        QStringLiteral("scripts/generate-bundle.bat"),
        QStringLiteral("scripts/generate-dmg.sh"),
        QStringLiteral("wix/Moonlight/Product.wxs"),
        QStringLiteral("wix/MoonlightSetup/Bundle.wxs"),
    };
    for (const QString& relativePath : packagingFiles) {
        QFile file(QDir(sourceRoot()).filePath(relativePath));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(relativePath));
        const QByteArray contents = file.readAll();
        QVERIFY2(!contents.contains("Moonlight.exe"),
                 qPrintable(relativePath));
        QVERIFY2(!contents.contains("Moonlight.app"),
                 qPrintable(relativePath));
    }

    QFile windowsScript(QDir(sourceRoot()).filePath(
        QStringLiteral("scripts/build-arch.bat")));
    QVERIFY(windowsScript.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(windowsScript.readAll().contains("Perigee.exe"));

    QFile macScript(QDir(sourceRoot()).filePath(
        QStringLiteral("scripts/generate-dmg.sh")));
    QVERIFY(macScript.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray macContents = macScript.readAll();
    QVERIFY(macContents.contains("Perigee.app/Contents/MacOS/Perigee"));
    QVERIFY(macContents.contains("Perigee-$VERSION.dmg"));

    QFile productProject(QDir(sourceRoot()).filePath(
        QStringLiteral("wix/Moonlight/Moonlight.wixproj")));
    QVERIFY(productProject.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(productProject.readAll().contains("<OutputName>Perigee</OutputName>"));

    QFile bundleProject(QDir(sourceRoot()).filePath(
        QStringLiteral("wix/MoonlightSetup/MoonlightSetup.wixproj")));
    QVERIFY(bundleProject.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(bundleProject.readAll().contains(
        "<OutputName>PerigeeSetup</OutputName>"));
}

void BrandingTest::crossPlatformPackagingUsesPerigeeIcons()
{
    struct IconExpectation {
        QString path;
        QByteArray signature;
    };
    const QVector<IconExpectation> icons {
        {QStringLiteral("app/perigee.ico"),
         QByteArray::fromHex("00000100")},
        {QStringLiteral("app/perigee.icns"), QByteArrayLiteral("icns")},
        {QStringLiteral("app/perigee_wix.png"),
         QByteArray::fromHex("89504e470d0a1a0a")},
    };
    for (const IconExpectation& icon : icons) {
        QFile file(QDir(sourceRoot()).filePath(icon.path));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(icon.path));
        QVERIFY2(file.size() > icon.signature.size(), qPrintable(icon.path));
        QCOMPARE(file.read(icon.signature.size()), icon.signature);
        const QByteArray remainder = file.readAll();
        QVERIFY2(!remainder.contains("tIME"), qPrintable(icon.path));
        QVERIFY2(!remainder.contains("date:create"), qPrintable(icon.path));
        QVERIFY2(!remainder.contains("date:modify"), qPrintable(icon.path));
    }

    const QImage installerLogo(QDir(sourceRoot()).filePath(
        QStringLiteral("app/perigee_wix.png")));
    QVERIFY(!installerLogo.isNull());
    QCOMPARE(installerLogo.size(), QSize(64, 64));

    QFile project(QDir(sourceRoot()).filePath(QStringLiteral("app/app.pro")));
    QVERIFY(project.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray projectContents = project.readAll();
    QVERIFY(projectContents.contains("RC_ICONS = perigee.ico"));
    QVERIFY(projectContents.contains("APP_BUNDLE_RESOURCES.files = perigee.icns"));
    QVERIFY(!projectContents.contains("RC_ICONS = moonlight.ico"));
    QVERIFY(!projectContents.contains("APP_BUNDLE_RESOURCES.files = moonlight.icns"));

    QFile plist(QDir(sourceRoot()).filePath(QStringLiteral("app/Info.plist")));
    QVERIFY(plist.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray plistContents = plist.readAll();
    QVERIFY(plistContents.contains("<key>CFBundleIconFile</key>\n\t<string>perigee</string>"));

    QFile icoGenerator(QDir(sourceRoot()).filePath(
        QStringLiteral("scripts/generate-ico.sh")));
    QVERIFY(icoGenerator.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray icoGeneratorContents = icoGenerator.readAll();
    QVERIFY(icoGeneratorContents.contains("res/perigee.svg"));
    QVERIFY(icoGeneratorContents.contains("perigee.ico"));
    QVERIFY(icoGeneratorContents.contains("perigee_wix.png"));
    QVERIFY(!icoGeneratorContents.contains("res/moonlight.svg"));

    QFile icnsGenerator(QDir(sourceRoot()).filePath(
        QStringLiteral("scripts/generate-icns.sh")));
    QVERIFY(icnsGenerator.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray icnsGeneratorContents = icnsGenerator.readAll();
    QVERIFY(icnsGeneratorContents.contains("res/perigee.svg"));
    QVERIFY(icnsGeneratorContents.contains("perigee.icns"));

    QFile bundle(QDir(sourceRoot()).filePath(
        QStringLiteral("wix/MoonlightSetup/Bundle.wxs")));
    QVERIFY(bundle.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray bundleContents = bundle.readAll();
    QVERIFY(bundleContents.contains("app\\perigee.ico"));
    QVERIFY(bundleContents.contains("app\\perigee_wix.png"));
    QVERIFY(!bundleContents.contains("app\\moonlight.ico"));
    QVERIFY(!bundleContents.contains("app\\moonlight_wix.png"));

    QFile theme(QDir(sourceRoot()).filePath(
        QStringLiteral("wix/MoonlightSetup/RtfTheme.xml")));
    QVERIFY(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray themeContents = theme.readAll();
    QVERIFY(themeContents.contains("IconFile=\"Perigee.ico\""));
    QVERIFY(!themeContents.contains("IconFile=\"Moonlight.ico\""));
}

void BrandingTest::allArtifactConsumersUsePerigeeNames()
{
    const QStringList auditedFiles {
        QStringLiteral(".github/workflows/build-win-mac.yml"),
        QStringLiteral(".github/workflows/build-steamlink.yml"),
        QStringLiteral(".github/workflows/build-appimage.yml"),
        QStringLiteral("scripts/build-steamlink-app.sh"),
        QStringLiteral("scripts/build-appimage.sh"),
        QStringLiteral("scripts/generate-src.sh"),
        QStringLiteral("app/deploy/steamlink/toc.txt"),
        QStringLiteral("app/deploy/steamlink/perigee.sh"),
    };
    for (const QString& relativePath : auditedFiles) {
        QFile file(QDir(sourceRoot()).filePath(relativePath));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(relativePath));
        const QByteArray contents = file.readAll();
        QVERIFY2(!contents.contains("Moonlight.exe"), qPrintable(relativePath));
        QVERIFY2(!contents.contains("Moonlight.app"), qPrintable(relativePath));
        QVERIFY2(!contents.contains("Moonlight-SteamLink"), qPrintable(relativePath));
        QVERIFY2(!contents.contains("Moonlight-LinuxAppImage"), qPrintable(relativePath));
        QVERIFY2(!contents.contains("MoonlightSrc"), qPrintable(relativePath));
        QVERIFY2(!contents.contains("app/moonlight"), qPrintable(relativePath));
        QVERIFY2(!contents.contains("bin/moonlight"), qPrintable(relativePath));
    }

    QFile winMacWorkflow(QDir(sourceRoot()).filePath(
        QStringLiteral(".github/workflows/build-win-mac.yml")));
    QVERIFY(winMacWorkflow.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray winMacContents = winMacWorkflow.readAll();
    QVERIFY(winMacContents.contains("name: Perigee-${{ runner.os }}-x64-"));
    QVERIFY(winMacContents.contains("name: Perigee-${{ runner.os }}-arm64-"));
    QVERIFY(winMacContents.contains("name: Perigee-DebugSymbols-${{ runner.os }}-"));

    QFile steamScript(QDir(sourceRoot()).filePath(
        QStringLiteral("scripts/build-steamlink-app.sh")));
    QVERIFY(steamScript.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray steamScriptContents = steamScript.readAll();
    QVERIFY(steamScriptContents.contains("app/perigee"));
    QVERIFY(steamScriptContents.contains("apps/perigee/bin"));
    QVERIFY(steamScriptContents.contains("Perigee-SteamLink-$VERSION.zip"));

    QFile steamWorkflow(QDir(sourceRoot()).filePath(
        QStringLiteral(".github/workflows/build-steamlink.yml")));
    QVERIFY(steamWorkflow.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray steamWorkflowContents = steamWorkflow.readAll();
    QVERIFY(steamWorkflowContents.contains("name: Perigee-SteamLink-"));
    QVERIFY(steamWorkflowContents.contains(
        "build/installer-release/Perigee-SteamLink-${{ env.CI_VERSION }}.zip"));

    QFile appImageWorkflow(QDir(sourceRoot()).filePath(
        QStringLiteral(".github/workflows/build-appimage.yml")));
    QVERIFY(appImageWorkflow.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray appImageWorkflowContents = appImageWorkflow.readAll();
    QVERIFY(appImageWorkflowContents.contains("name: Perigee-LinuxAppImage-"));
    QVERIFY(appImageWorkflowContents.contains(
        "build/installer-release/Perigee-${{ env.CI_VERSION }}-x86_64.AppImage"));

    QFile toc(QDir(sourceRoot()).filePath(
        QStringLiteral("app/deploy/steamlink/toc.txt")));
    QVERIFY(toc.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray tocContents = toc.readAll();
    QVERIFY(tocContents.contains("name=Perigee"));
    QVERIFY(tocContents.contains("icon=perigee.png"));
    QVERIFY(tocContents.contains("run=perigee.sh"));

    const QString steamIconPath = QDir(sourceRoot()).filePath(
        QStringLiteral("app/deploy/steamlink/perigee.png"));
    const QImage steamIcon(steamIconPath);
    QVERIFY2(!steamIcon.isNull(), qPrintable(steamIconPath));
    QCOMPARE(steamIcon.size(), QSize(116, 116));
    QVERIFY(!QFileInfo::exists(QDir(sourceRoot()).filePath(
        QStringLiteral("app/deploy/steamlink/moonlight.png"))));
    QVERIFY(!QFileInfo::exists(QDir(sourceRoot()).filePath(
        QStringLiteral("app/deploy/steamlink/moonlight.sh"))));
}

void BrandingTest::iconGeneratorFailsWhenSourceIsMissing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("scripts"))));

    const QString source = QDir(sourceRoot()).filePath(
        QStringLiteral("scripts/generate-ico.sh"));
    const QString isolatedScript = directory.filePath(
        QStringLiteral("scripts/generate-ico.sh"));
    QVERIFY(QFile::copy(source, isolatedScript));

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.start(QStringLiteral("/bin/sh"), {isolatedScript});
    QVERIFY2(process.waitForFinished(10000), qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() != 0, process.readAll().constData());
    QVERIFY(!QFileInfo::exists(directory.filePath(
        QStringLiteral("app/perigee.ico"))));
    QVERIFY(!QFileInfo::exists(directory.filePath(
        QStringLiteral("app/perigee_wix.png"))));
}

void BrandingTest::githubSupportSurfaceUsesPerigeeIdentity()
{
    QFile bugReport(QDir(sourceRoot()).filePath(
        QStringLiteral(".github/ISSUE_TEMPLATE/bug_report.md")));
    QVERIFY(bugReport.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray bugReportContents = bugReport.readAll();

    QFile autoComment(QDir(sourceRoot()).filePath(
        QStringLiteral(".github/auto-comment.yml")));
    QVERIFY(autoComment.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray autoCommentContents = autoComment.readAll();
    const QString combined = QString::fromUtf8(
        bugReportContents + autoCommentContents);

    QVERIFY(bugReportContents.contains("Perigee version"));
    QVERIFY(bugReportContents.contains("Perigee settings"));
    QVERIFY(bugReportContents.contains("Perigee logs"));
    QVERIFY(bugReportContents.contains("Perigee-*.log"));
    QVERIFY(bugReportContents.contains("Linux development builds"));
    QVERIFY(bugReportContents.contains("terminal output"));

    QVERIFY(!combined.contains(QStringLiteral("Moonlight-###.log")));
    QVERIFY(!combined.contains(QStringLiteral("flatpak run")));
    QVERIFY(!combined.contains(QStringLiteral("com.moonlight_stream.Moonlight")));
    QVERIFY(!combined.contains(QRegularExpression(
        QStringLiteral("\\bSnap\\b"),
        QRegularExpression::CaseInsensitiveOption)));
    QVERIFY(!combined.contains(QRegularExpression(
        QStringLiteral("\\bour\\s+(Discord|support)\\b"),
        QRegularExpression::CaseInsensitiveOption)));

    if (bugReportContents.contains("moonlight-stream/moonlight-docs")) {
        QVERIFY(bugReportContents.contains(
            "upstream Moonlight troubleshooting guide"));
        QVERIFY(bugReportContents.contains(
            "not Perigee-specific support"));
    }
    if (combined.contains(QStringLiteral("moonlight-stream.org/discord"))) {
        QVERIFY(combined.contains(QStringLiteral("upstream Moonlight Discord")));
        QVERIFY(combined.contains(QStringLiteral("not Perigee-specific support")));
    }

    QVERIFY(bugReportContents.contains(
        "Comparison with upstream Moonlight clients (optional)"));
    QVERIFY(bugReportContents.contains("inherited Moonlight-core behavior"));
    QVERIFY(autoCommentContents.contains("Perigee issue tracker"));
}

void BrandingTest::declinedImportMovesNoProfileData()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);
    legacy.setValue(QStringLiteral("width"), 1920);
    legacy.setValue(QStringLiteral("unknown"), QStringLiteral("do-not-copy"));

    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(migration.decisionRequired());
    QVERIFY(migration.declineImport());
    QCOMPARE(perigee.allKeys(),
             QStringList({QStringLiteral("migration/moonlightImportDecision")}));
    QCOMPARE(perigee.value(QStringLiteral("migration/moonlightImportDecision")),
             QVariant(QStringLiteral("declined")));
    QVERIFY(!MoonlightSettingsImport(legacy, perigee).decisionRequired());
}

void BrandingTest::consentImportsOnlyRecognizedDataAndPairingIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);

    legacy.setValue(QStringLiteral("width"), 2560);
    legacy.setValue(QStringLiteral("bitrate"), 50000);
    legacy.setValue(QStringLiteral("deckKeyModifiers"), 123);
    legacy.setValue(QStringLiteral("logPath"), QStringLiteral("/tmp/private.log"));
    legacy.setValue(QStringLiteral("unknown"), QStringLiteral("do-not-copy"));
    legacy.setValue(QStringLiteral("certificate"), QByteArrayLiteral("TEST CERTIFICATE"));
    legacy.setValue(QStringLiteral("key"), QByteArrayLiteral("TEST PRIVATE KEY"));
    legacy.setValue(QStringLiteral("uniqueid"), QStringLiteral("TEST-UNIQUE-ID"));

    legacy.beginWriteArray(QStringLiteral("hosts"));
    legacy.setArrayIndex(0);
    legacy.setValue(QStringLiteral("hostname"), QStringLiteral("Test host"));
    legacy.setValue(QStringLiteral("uuid"), QStringLiteral("test-host-uuid"));
    legacy.setValue(QStringLiteral("srvcert"), QByteArrayLiteral("TEST SERVER CERTIFICATE"));
    legacy.setValue(QStringLiteral("manualaddress"), QStringLiteral("192.0.2.10"));
    legacy.setValue(QStringLiteral("unknownHostValue"), QStringLiteral("do-not-copy"));
    legacy.beginWriteArray(QStringLiteral("apps"));
    legacy.setArrayIndex(0);
    legacy.setValue(QStringLiteral("name"), QStringLiteral("Desktop"));
    legacy.setValue(QStringLiteral("id"), 7);
    legacy.setValue(QStringLiteral("unknownAppValue"), QStringLiteral("do-not-copy"));
    legacy.endArray();
    legacy.endArray();
    legacy.sync();

    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(migration.decisionRequired());
    QVERIFY(migration.acceptImport());
    QCOMPARE(perigee.value(QStringLiteral("width")).toInt(), 2560);
    QCOMPARE(perigee.value(QStringLiteral("bitrate")).toInt(), 50000);
    QVERIFY(!perigee.contains(QStringLiteral("deckKeyModifiers")));
    QVERIFY(!perigee.contains(QStringLiteral("logPath")));
    QVERIFY(!perigee.contains(QStringLiteral("unknown")));
    QCOMPARE(perigee.value(QStringLiteral("certificate")).toByteArray(),
             QByteArrayLiteral("TEST CERTIFICATE"));
    QCOMPARE(perigee.value(QStringLiteral("key")).toByteArray(),
             QByteArrayLiteral("TEST PRIVATE KEY"));
    QCOMPARE(perigee.value(QStringLiteral("uniqueid")).toString(),
             QStringLiteral("TEST-UNIQUE-ID"));

    QCOMPARE(perigee.beginReadArray(QStringLiteral("hosts")), 1);
    perigee.setArrayIndex(0);
    QCOMPARE(perigee.value(QStringLiteral("hostname")).toString(),
             QStringLiteral("Test host"));
    QCOMPARE(perigee.value(QStringLiteral("uuid")).toString(),
             QStringLiteral("test-host-uuid"));
    QVERIFY(!perigee.contains(QStringLiteral("unknownHostValue")));
    QCOMPARE(perigee.beginReadArray(QStringLiteral("apps")), 1);
    perigee.setArrayIndex(0);
    QCOMPARE(perigee.value(QStringLiteral("name")).toString(),
             QStringLiteral("Desktop"));
    QCOMPARE(perigee.value(QStringLiteral("id")).toInt(), 7);
    QVERIFY(!perigee.contains(QStringLiteral("unknownAppValue")));
    perigee.endArray();
    perigee.endArray();
}

void BrandingTest::handledDecisionIsIdempotent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);
    legacy.setValue(QStringLiteral("width"), 1920);

    MoonlightSettingsImport first(legacy, perigee);
    QVERIFY(first.acceptImport());
    QCOMPARE(perigee.value(QStringLiteral("width")).toInt(), 1920);

    legacy.setValue(QStringLiteral("width"), 3840);
    MoonlightSettingsImport second(legacy, perigee);
    QVERIFY(!second.decisionRequired());
    QVERIFY(!second.acceptImport());
    QCOMPARE(perigee.value(QStringLiteral("width")).toInt(), 1920);
}

void BrandingTest::importDoesNotOverwriteInitializedPerigeeProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);
    legacy.setValue(QStringLiteral("width"), 3840);
    perigee.setValue(QStringLiteral("width"), 1280);
    perigee.setValue(QStringLiteral("certificate"), QByteArrayLiteral("NEW CERTIFICATE"));

    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(!migration.decisionRequired());
    QVERIFY(!migration.acceptImport());
    QCOMPARE(perigee.value(QStringLiteral("width")).toInt(), 1280);
    QCOMPARE(perigee.value(QStringLiteral("certificate")).toByteArray(),
             QByteArrayLiteral("NEW CERTIFICATE"));
}

void BrandingTest::pairingIdentityRequiresAnImportablePairedHost()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);
    legacy.setValue(QStringLiteral("width"), 1920);
    legacy.setValue(QStringLiteral("certificate"), QByteArrayLiteral("TEST CERTIFICATE"));
    legacy.setValue(QStringLiteral("key"), QByteArrayLiteral("TEST PRIVATE KEY"));
    legacy.setValue(QStringLiteral("uniqueid"), QStringLiteral("TEST-UNIQUE-ID"));

    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(migration.decisionRequired());
    QVERIFY(migration.acceptImport());
    QCOMPARE(perigee.value(QStringLiteral("width")).toInt(), 1920);
    QVERIFY(!perigee.contains(QStringLiteral("certificate")));
    QVERIFY(!perigee.contains(QStringLiteral("key")));
    QVERIFY(!perigee.contains(QStringLiteral("uniqueid")));
}

void BrandingTest::incompletePairingIdentityIsNotImportable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);
    legacy.setValue(QStringLiteral("certificate"), QByteArrayLiteral("TEST CERTIFICATE"));
    legacy.setValue(QStringLiteral("key"), QByteArrayLiteral("TEST PRIVATE KEY"));
    legacy.beginWriteArray(QStringLiteral("hosts"));
    legacy.setArrayIndex(0);
    legacy.setValue(QStringLiteral("hostname"), QStringLiteral("Incomplete host"));
    legacy.setValue(QStringLiteral("uuid"), QStringLiteral("incomplete-host"));
    legacy.setValue(QStringLiteral("srvcert"), QByteArrayLiteral("TEST SERVER CERTIFICATE"));
    legacy.endArray();
    legacy.sync();

    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(!migration.decisionRequired());
    QVERIFY(!migration.acceptImport());
    QCOMPARE(perigee.allKeys(), QStringList());
}

void BrandingTest::interruptedImportKeepsDurableRecoveryMarkerAcrossConsecutiveFailures()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    legacy.setValue(QStringLiteral("width"), 2560);
    legacy.setValue(QStringLiteral("bitrate"), 50000);
    legacy.sync();

    settingsWriteFailures = {false, true, true, true, true, true};
    const QString destination =
        directory.filePath(QStringLiteral("perigee.failed-sync"));
    QFile placeholder(destination);
    QVERIFY(placeholder.open(QIODevice::WriteOnly));
    placeholder.close();

    {
        QSettings perigee(destination, failedSyncSettingsFormat());
        MoonlightSettingsImport migration(legacy, perigee);
        QVERIFY(migration.decisionRequired());
        QVERIFY(!migration.acceptImport());
        QVERIFY(migration.decisionRequired());
        QVERIFY(!migration.acceptImport());
        QVERIFY(migration.decisionRequired());

        const QSettings::SettingsMap durableMarker =
            readFailedSyncSettingsFile(destination);
        QCOMPARE(durableMarker.value(QStringLiteral(
                     "migration/moonlightImportTransactionPhase")),
                 QVariant(QStringLiteral("prepared")));
        QVERIFY(!durableMarker.contains(QStringLiteral("width")));
        QVERIFY(!durableMarker.contains(QStringLiteral("bitrate")));

        settingsWriteFailures.clear();
        QVERIFY(migration.acceptImport());
    }

    QSettings persisted(destination, failedSyncSettingsFormat());
    QCOMPARE(persisted.value(QStringLiteral("width")).toInt(), 2560);
    QCOMPARE(persisted.value(QStringLiteral("bitrate")).toInt(), 50000);
    QCOMPARE(persisted.value(QStringLiteral("migration/moonlightImportDecision")),
             QVariant(QStringLiteral("imported")));
    QVERIFY(!persisted.contains(QStringLiteral(
        "migration/moonlightImportTransactionPhase")));
}

void BrandingTest::coldReopenBlocksPartialProfileUntilRecoveryCanPersist()
{
    if (!qEnvironmentVariableIsSet("PERIGEE_COLD_RECOVERY_PROBE")) {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString legacyPath = directory.filePath(
            QStringLiteral("moonlight.ini"));
        const QString destination = directory.filePath(
            QStringLiteral("perigee.failed-sync"));

        {
            QSettings legacy(legacyPath, QSettings::IniFormat);
            legacy.setValue(QStringLiteral("width"), 1920);
            legacy.sync();
            QSettings partial(destination, failedSyncSettingsFormat());
            partial.setValue(QStringLiteral(
                "migration/moonlightImportTransactionPhase"),
                QStringLiteral("prepared"));
            partial.setValue(QStringLiteral("width"), 4096);
            partial.setValue(QStringLiteral("certificate"),
                             QByteArrayLiteral("PARTIAL CERTIFICATE"));
            partial.setValue(QStringLiteral("key"),
                             QByteArrayLiteral("PARTIAL PRIVATE KEY"));
            partial.setValue(QStringLiteral("uniqueid"),
                             QStringLiteral("PARTIAL-IDENTITY"));
            partial.sync();
            QCOMPARE(partial.status(), QSettings::NoError);
        }

        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PERIGEE_COLD_RECOVERY_PROBE"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("PERIGEE_STARTUP_GATE_PROBE"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("PERIGEE_SETTINGS_WRITE_FAILURES"),
                           QStringLiteral("100"));
        environment.insert(QStringLiteral("PERIGEE_LEGACY_SETTINGS_PATH"),
                           legacyPath);
        environment.insert(QStringLiteral("PERIGEE_PARTIAL_SETTINGS_PATH"),
                           destination);
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                           QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("QT_QUICK_BACKEND"),
                           QStringLiteral("software"));
        environment.insert(QStringLiteral("QML_DISABLE_DISK_CACHE"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("SDL_VIDEODRIVER"),
                           QStringLiteral("dummy"));
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("BrandingTest"),
                       QStringLiteral(
                           "coldReopenBlocksPartialProfileUntilRecoveryCanPersist"),
                       QStringLiteral("-silent")});
        QVERIFY2(process.waitForFinished(10000),
                 qPrintable(process.errorString()));
        const QByteArray output = process.readAll();
        QVERIFY2(process.exitStatus() == QProcess::NormalExit,
                 output.constData());
        QVERIFY2(process.exitCode() == 0, output.constData());

        const QSettings::SettingsMap persisted =
            readFailedSyncSettingsFile(destination);
        QCOMPARE(persisted.value(QStringLiteral(
                     "migration/moonlightImportTransactionPhase")),
                 QVariant(QStringLiteral("prepared")));
        QCOMPARE(persisted.value(QStringLiteral("width")).toInt(), 4096);
        QCOMPARE(persisted.value(QStringLiteral("uniqueid")).toString(),
                 QStringLiteral("PARTIAL-IDENTITY"));
        return;
    }

    startupGateCounts = {};
    QSettings legacy(qEnvironmentVariable("PERIGEE_LEGACY_SETTINGS_PATH"),
                     QSettings::IniFormat);
    QSettings perigee(qEnvironmentVariable("PERIGEE_PARTIAL_SETTINGS_PATH"),
                      failedSyncSettingsFormat());
    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(migration.decisionRequired());
    QVERIFY(!migration.acceptImport());
    QVERIFY(migration.decisionRequired());

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("MoonlightSettingsImport"), &migration);
    engine.rootContext()->setContextProperty(
        QStringLiteral("applicationWindowTitle"), QStringLiteral("Perigee"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("initialView"), QStringLiteral("qrc:/gui/PcView.qml"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("runConfigChecks"), false);
    engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
    QVERIFY2(!engine.rootObjects().isEmpty(), "main.qml did not load");
    QCOMPARE(startupGateCounts.computerManagerCreations, 0);
    QCOMPARE(startupGateCounts.preferenceCreations, 0);
    QCOMPARE(startupGateCounts.navigationCreations, 0);
    QCOMPARE(startupGateCounts.pollingStarts, 0);
    QCOMPARE(startupGateCounts.focusNotifications, 0);
}

void BrandingTest::importPromptDoesNotInitializeStreamingState()
{
    if (!qEnvironmentVariableIsSet("PERIGEE_STARTUP_GATE_PROBE")) {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PERIGEE_STARTUP_GATE_PROBE"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                           QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("QT_QUICK_BACKEND"),
                           QStringLiteral("software"));
        environment.insert(QStringLiteral("QML_DISABLE_DISK_CACHE"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("SDL_VIDEODRIVER"),
                           QStringLiteral("dummy"));
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("BrandingTest"),
                       QStringLiteral("importPromptDoesNotInitializeStreamingState"),
                       QStringLiteral("-silent")});
        QVERIFY2(process.waitForFinished(10000),
                 qPrintable(process.errorString()));
        const QByteArray output = process.readAll();
        QVERIFY2(process.exitStatus() == QProcess::NormalExit,
                 output.constData());
        QVERIFY2(process.exitCode() == 0, output.constData());
        return;
    }

    startupGateCounts = {};
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings legacy(directory.filePath(QStringLiteral("moonlight.ini")),
                     QSettings::IniFormat);
    QSettings perigee(directory.filePath(QStringLiteral("perigee.ini")),
                      QSettings::IniFormat);
    legacy.setValue(QStringLiteral("width"), 1920);
    MoonlightSettingsImport migration(legacy, perigee);
    QVERIFY(migration.decisionRequired());

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("MoonlightSettingsImport"), &migration);
    engine.rootContext()->setContextProperty(
        QStringLiteral("applicationWindowTitle"), QStringLiteral("Perigee"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("initialView"), QStringLiteral("qrc:/gui/PcView.qml"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("runConfigChecks"), false);
    engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
    QVERIFY2(!engine.rootObjects().isEmpty(), "main.qml did not load");

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    QVERIFY(window);
    QVERIFY(window->isVisible());

    QQuickItem* focusedItem = window->activeFocusItem();
    QVERIFY(focusedItem);
    QString focusedText = focusedItem->property("text").toString();
    focusedText.remove(QLatin1Char('&'));
    QCOMPARE(focusedText, QStringLiteral("No"));

    QCOMPARE(startupGateCounts.computerManagerCreations, 0);
    QCOMPARE(startupGateCounts.preferenceCreations, 0);
    QCOMPARE(startupGateCounts.navigationCreations, 0);
    QCOMPARE(startupGateCounts.pollingStarts, 0);
    QCOMPARE(startupGateCounts.focusNotifications, 0);
    QVERIFY(migration.decisionRequired());
    QCOMPARE(perigee.allKeys(), QStringList());
}

REGISTER_PERIGEE_TEST(BrandingTest);

#include "test_branding.moc"
