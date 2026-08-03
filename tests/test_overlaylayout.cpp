#include "test_registry.h"

#include "streaming/video/overlaymanager.h"

#include <QtTest>

class RecordingOverlayRenderer final : public Overlay::IOverlayRenderer
{
public:
    void notifyOverlayUpdated(Overlay::OverlayType type) override
    {
        notifications.push_back(type);
    }

    QVector<Overlay::OverlayType> notifications;
};

class OverlayLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void placesAndConstrainsOverlays_data();
    void placesAndConstrainsOverlays();
    void replacesPendingSurfaceAndPresentation();
    void representsNullSurfaceAsAConsumableClear();
    void consumedSurfaceOutlivesManager();
    void disablingDeckPublishesClearWithoutTextRendering();
};

void OverlayLayoutTest::placesAndConstrainsOverlays_data()
{
    QTest::addColumn<int>("anchor");
    QTest::addColumn<int>("marginPx");
    QTest::addColumn<float>("maxWidthRatio");
    QTest::addColumn<float>("maxHeightRatio");
    QTest::addColumn<int>("surfaceWidth");
    QTest::addColumn<int>("surfaceHeight");
    QTest::addColumn<int>("viewportWidth");
    QTest::addColumn<int>("viewportHeight");
    QTest::addColumn<bool>("originAtBottomLeft");
    QTest::addColumn<float>("expectedX");
    QTest::addColumn<float>("expectedY");
    QTest::addColumn<float>("expectedWidth");
    QTest::addColumn<float>("expectedHeight");

    QTest::newRow("Deck top-center at intrinsic size")
        << static_cast<int>(Overlay::OverlayAnchor::TopCenter)
        << 0 << 1.0f << 1.0f
        << 640 << 360 << 1920 << 1080 << false
        << 640.0f << 0.0f << 640.0f << 360.0f;

    QTest::newRow("Deck proportionally clamped to viewport")
        << static_cast<int>(Overlay::OverlayAnchor::TopCenter)
        << 0 << 1.0f << 1.0f
        << 640 << 360 << 480 << 270 << false
        << 0.0f << 0.0f << 480.0f << 270.0f;

    QTest::newRow("fractional high-DPI constrained placement")
        << static_cast<int>(Overlay::OverlayAnchor::TopCenter)
        << 0 << 0.75f << 1.0f
        << 1280 << 720 << 1365 << 768 << false
        << 170.625f << 0.0f << 1023.75f << 575.859375f;

    QTest::newRow("legacy status text bottom-left")
        << static_cast<int>(Overlay::OverlayAnchor::BottomLeft)
        << 0 << 1.0f << 1.0f
        << 512 << 48 << 1920 << 1080 << false
        << 0.0f << 1032.0f << 512.0f << 48.0f;

    QTest::newRow("legacy status text bottom-left with OpenGL origin")
        << static_cast<int>(Overlay::OverlayAnchor::BottomLeft)
        << 0 << 1.0f << 1.0f
        << 512 << 48 << 1920 << 1080 << true
        << 0.0f << 0.0f << 512.0f << 48.0f;
}

void OverlayLayoutTest::placesAndConstrainsOverlays()
{
    QFETCH(int, anchor);
    QFETCH(int, marginPx);
    QFETCH(float, maxWidthRatio);
    QFETCH(float, maxHeightRatio);
    QFETCH(int, surfaceWidth);
    QFETCH(int, surfaceHeight);
    QFETCH(int, viewportWidth);
    QFETCH(int, viewportHeight);
    QFETCH(bool, originAtBottomLeft);
    QFETCH(float, expectedX);
    QFETCH(float, expectedY);
    QFETCH(float, expectedWidth);
    QFETCH(float, expectedHeight);

    const Overlay::OverlayPresentation presentation {
        static_cast<Overlay::OverlayAnchor>(anchor),
        marginPx,
        maxWidthRatio,
        maxHeightRatio,
    };

    const SDL_FRect actual = Overlay::calculateOverlayRect(
        presentation,
        surfaceWidth,
        surfaceHeight,
        viewportWidth,
        viewportHeight,
        originAtBottomLeft);

    QVERIFY(qFuzzyCompare(actual.x + 1.0f, expectedX + 1.0f));
    QVERIFY(qFuzzyCompare(actual.y + 1.0f, expectedY + 1.0f));
    QVERIFY(qFuzzyCompare(actual.w + 1.0f, expectedWidth + 1.0f));
    QVERIFY(qFuzzyCompare(actual.h + 1.0f, expectedHeight + 1.0f));
}

void OverlayLayoutTest::replacesPendingSurfaceAndPresentation()
{
    Overlay::OverlayManager manager;
    RecordingOverlayRenderer renderer;
    manager.setOverlayRenderer(&renderer);

    SDL_Surface* first = SDL_CreateRGBSurfaceWithFormat(
        0, 16, 9, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface* second = SDL_CreateRGBSurfaceWithFormat(
        0, 32, 18, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);

    manager.updateOverlaySurface(
        Overlay::OverlayDeck,
        first,
        {Overlay::OverlayAnchor::TopLeft, 3, 0.5f, 0.5f});
    manager.updateOverlaySurface(
        Overlay::OverlayDeck,
        second,
        {Overlay::OverlayAnchor::TopCenter, 7, 0.8f, 0.6f});

    QCOMPARE(renderer.notifications,
             QVector<Overlay::OverlayType>({Overlay::OverlayDeck, Overlay::OverlayDeck}));

    SDL_Surface* consumed = nullptr;
    Overlay::OverlayPresentation presentation;
    QVERIFY(manager.getUpdatedOverlaySurface(
        Overlay::OverlayDeck, &consumed, &presentation));
    QCOMPARE(consumed, second);
    QCOMPARE(presentation.anchor, Overlay::OverlayAnchor::TopCenter);
    QCOMPARE(presentation.marginPx, 7);
    QCOMPARE(presentation.maxWidthRatio, 0.8f);
    QCOMPARE(presentation.maxHeightRatio, 0.6f);

    SDL_FreeSurface(consumed);
}

void OverlayLayoutTest::representsNullSurfaceAsAConsumableClear()
{
    Overlay::OverlayManager manager;
    const Overlay::OverlayPresentation clearPresentation {
        Overlay::OverlayAnchor::BottomLeft, 11, 0.9f, 0.7f};

    manager.updateOverlaySurface(
        Overlay::OverlayStatusUpdate, nullptr, clearPresentation);

    SDL_Surface* consumed = reinterpret_cast<SDL_Surface*>(quintptr(1));
    Overlay::OverlayPresentation presentation;
    QVERIFY(manager.getUpdatedOverlaySurface(
        Overlay::OverlayStatusUpdate, &consumed, &presentation));
    QCOMPARE(consumed, nullptr);
    QCOMPARE(presentation.anchor, Overlay::OverlayAnchor::BottomLeft);
    QCOMPARE(presentation.marginPx, 11);
    QCOMPARE(presentation.maxWidthRatio, 0.9f);
    QCOMPARE(presentation.maxHeightRatio, 0.7f);

    consumed = reinterpret_cast<SDL_Surface*>(quintptr(1));
    QVERIFY(!manager.getUpdatedOverlaySurface(
        Overlay::OverlayStatusUpdate, &consumed, &presentation));
    QCOMPARE(consumed, nullptr);
}

void OverlayLayoutTest::consumedSurfaceOutlivesManager()
{
    SDL_Surface* consumed = nullptr;
    {
        Overlay::OverlayManager manager;
        SDL_Surface* submitted = SDL_CreateRGBSurfaceWithFormat(
            0, 37, 19, 32, SDL_PIXELFORMAT_ARGB8888);
        QVERIFY(submitted != nullptr);

        manager.updateOverlaySurface(
            Overlay::OverlayDeck,
            submitted,
            {Overlay::OverlayAnchor::TopCenter, 0, 1.0f, 1.0f});

        Overlay::OverlayPresentation presentation;
        QVERIFY(manager.getUpdatedOverlaySurface(
            Overlay::OverlayDeck, &consumed, &presentation));
    }

    QVERIFY(consumed != nullptr);
    QCOMPARE(consumed->w, 37);
    QCOMPARE(consumed->h, 19);
    SDL_FreeSurface(consumed);
}

void OverlayLayoutTest::disablingDeckPublishesClearWithoutTextRendering()
{
    Overlay::OverlayManager manager;
    RecordingOverlayRenderer renderer;
    manager.setOverlayRenderer(&renderer);

    manager.setOverlayState(Overlay::OverlayDeck, true);
    QCOMPARE(renderer.notifications.size(), 0);

    SDL_Surface* submitted = SDL_CreateRGBSurfaceWithFormat(
        0, 64, 36, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(submitted != nullptr);
    manager.updateOverlaySurface(
        Overlay::OverlayDeck,
        submitted,
        {Overlay::OverlayAnchor::TopCenter, 0, 1.0f, 1.0f});

    SDL_Surface* consumed = nullptr;
    Overlay::OverlayPresentation presentation;
    QVERIFY(manager.getUpdatedOverlaySurface(
        Overlay::OverlayDeck, &consumed, &presentation));
    SDL_FreeSurface(consumed);
    renderer.notifications.clear();

    manager.setOverlayState(Overlay::OverlayDeck, false);
    QCOMPARE(renderer.notifications,
             QVector<Overlay::OverlayType>({Overlay::OverlayDeck}));
    QVERIFY(manager.getUpdatedOverlaySurface(
        Overlay::OverlayDeck, &consumed, &presentation));
    QCOMPARE(consumed, nullptr);
}

REGISTER_PERIGEE_TEST(OverlayLayoutTest);

#include "test_overlaylayout.moc"
