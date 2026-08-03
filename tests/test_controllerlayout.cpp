#include "test_registry.h"

#include "perigee/input/controllerlayout.h"

#include <QFile>
#include <QtTest>

class ControllerLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void mapsLogicalActionsToPhysicalArtwork_data();
    void mapsLogicalActionsToPhysicalArtwork();
    void detectsSupportedControllerFamilies_data();
    void detectsSupportedControllerFamilies();
};

void ControllerLayoutTest::mapsLogicalActionsToPhysicalArtwork_data()
{
    QTest::addColumn<int>("family");
    QTest::addColumn<bool>("swapFaceButtons");
    QTest::addColumn<QString>("confirmLabel");
    QTest::addColumn<QString>("backLabel");
    QTest::addColumn<QString>("searchLabel");
    QTest::addColumn<QString>("previousCategoryLabel");
    QTest::addColumn<QString>("nextCategoryLabel");
    QTest::addColumn<QString>("confirmGlyph");
    QTest::addColumn<QString>("backGlyph");

    using Family = ControllerLayout::Family;
    QTest::newRow("xbox")
        << int(Family::Xbox) << false
        << QStringLiteral("A") << QStringLiteral("B")
        << QStringLiteral("Y") << QStringLiteral("LB")
        << QStringLiteral("RB")
        << QStringLiteral("qrc:/gui/perigee/glyphs/xbox-a.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/xbox-b.svg");
    QTest::newRow("playstation")
        << int(Family::PlayStation) << false
        << QStringLiteral("Cross") << QStringLiteral("Circle")
        << QStringLiteral("Triangle") << QStringLiteral("L1")
        << QStringLiteral("R1")
        << QStringLiteral("qrc:/gui/perigee/glyphs/playstation-cross.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/playstation-circle.svg");
    QTest::newRow("nintendo")
        << int(Family::Nintendo) << false
        << QStringLiteral("B") << QStringLiteral("A")
        << QStringLiteral("X") << QStringLiteral("L")
        << QStringLiteral("R")
        << QStringLiteral("qrc:/gui/perigee/glyphs/nintendo-b.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/nintendo-a.svg");
    QTest::newRow("steam-deck")
        << int(Family::SteamDeck) << false
        << QStringLiteral("A") << QStringLiteral("B")
        << QStringLiteral("Y") << QStringLiteral("L1")
        << QStringLiteral("R1")
        << QStringLiteral("qrc:/gui/perigee/glyphs/steamdeck-a.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/steamdeck-b.svg");
    QTest::newRow("unknown-falls-back-to-xbox")
        << int(Family::Unknown) << false
        << QStringLiteral("A") << QStringLiteral("B")
        << QStringLiteral("Y") << QStringLiteral("LB")
        << QStringLiteral("RB")
        << QStringLiteral("qrc:/gui/perigee/glyphs/xbox-a.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/xbox-b.svg");
    QTest::newRow("xbox-swapped")
        << int(Family::Xbox) << true
        << QStringLiteral("B") << QStringLiteral("A")
        << QStringLiteral("X") << QStringLiteral("LB")
        << QStringLiteral("RB")
        << QStringLiteral("qrc:/gui/perigee/glyphs/xbox-b.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/xbox-a.svg");
    QTest::newRow("playstation-swapped")
        << int(Family::PlayStation) << true
        << QStringLiteral("Circle") << QStringLiteral("Cross")
        << QStringLiteral("Square") << QStringLiteral("L1")
        << QStringLiteral("R1")
        << QStringLiteral("qrc:/gui/perigee/glyphs/playstation-circle.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/playstation-cross.svg");
    QTest::newRow("nintendo-swapped")
        << int(Family::Nintendo) << true
        << QStringLiteral("A") << QStringLiteral("B")
        << QStringLiteral("Y") << QStringLiteral("L")
        << QStringLiteral("R")
        << QStringLiteral("qrc:/gui/perigee/glyphs/nintendo-a.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/nintendo-b.svg");

    QTest::newRow("steam-deck-swapped")
        << int(Family::SteamDeck) << true
        << QStringLiteral("B") << QStringLiteral("A")
        << QStringLiteral("X") << QStringLiteral("L1")
        << QStringLiteral("R1")
        << QStringLiteral("qrc:/gui/perigee/glyphs/steamdeck-b.svg")
        << QStringLiteral("qrc:/gui/perigee/glyphs/steamdeck-a.svg");
}

void ControllerLayoutTest::mapsLogicalActionsToPhysicalArtwork()
{
    QFETCH(int, family);
    QFETCH(bool, swapFaceButtons);
    QFETCH(QString, confirmLabel);
    QFETCH(QString, backLabel);
    QFETCH(QString, searchLabel);
    QFETCH(QString, previousCategoryLabel);
    QFETCH(QString, nextCategoryLabel);
    QFETCH(QString, confirmGlyph);
    QFETCH(QString, backGlyph);

    ControllerLayout layout;
    layout.configure(static_cast<ControllerLayout::Family>(family),
                     swapFaceButtons);

    QCOMPARE(layout.confirmLabel(), confirmLabel);
    QCOMPARE(layout.backLabel(), backLabel);
    QCOMPARE(layout.searchLabel(), searchLabel);
    QCOMPARE(layout.previousCategoryLabel(), previousCategoryLabel);
    QCOMPARE(layout.nextCategoryLabel(), nextCategoryLabel);
    QCOMPARE(layout.confirmGlyph().toString(), confirmGlyph);
    QCOMPARE(layout.backGlyph().toString(), backGlyph);
    QVERIFY2(QFile::exists(layout.confirmGlyph().toString().mid(3)),
             qPrintable(layout.confirmGlyph().toString()));
    QVERIFY2(QFile::exists(layout.backGlyph().toString().mid(3)),
             qPrintable(layout.backGlyph().toString()));
}

void ControllerLayoutTest::detectsSupportedControllerFamilies_data()
{
    QTest::addColumn<int>("sdlType");
    QTest::addColumn<QString>("deviceName");
    QTest::addColumn<int>("expectedFamily");

    using Family = ControllerLayout::Family;
    QTest::newRow("xbox-one")
        << int(SDL_CONTROLLER_TYPE_XBOXONE) << QStringLiteral("Xbox Controller")
        << int(Family::Xbox);
    QTest::newRow("playstation-5")
        << int(SDL_CONTROLLER_TYPE_PS5) << QStringLiteral("DualSense")
        << int(Family::PlayStation);
    QTest::newRow("switch-pro")
        << int(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO)
        << QStringLiteral("Nintendo Switch Pro Controller")
        << int(Family::Nintendo);
    QTest::newRow("steam-deck-name-wins")
        << int(SDL_CONTROLLER_TYPE_UNKNOWN)
        << QStringLiteral("Valve Steam Deck Controller")
        << int(Family::SteamDeck);
    QTest::newRow("unknown")
        << int(SDL_CONTROLLER_TYPE_UNKNOWN) << QStringLiteral("USB Gamepad")
        << int(Family::Unknown);
}

void ControllerLayoutTest::detectsSupportedControllerFamilies()
{
    QFETCH(int, sdlType);
    QFETCH(QString, deviceName);
    QFETCH(int, expectedFamily);

    QCOMPARE(int(ControllerLayout::familyForSdl(
                 static_cast<SDL_GameControllerType>(sdlType), deviceName)),
             expectedFamily);
}

REGISTER_PERIGEE_TEST(ControllerLayoutTest);

#include "test_controllerlayout.moc"
