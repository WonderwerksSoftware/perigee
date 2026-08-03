#include "test_registry.h"

#include "streaming/video/overlaymanager.h"

#include <QtTest>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

class RecordingOverlayRenderer final : public Overlay::IOverlayRenderer
{
public:
    void notifyOverlayUpdated(Overlay::OverlayType type) override
    {
        notifications.push_back(type);
    }

    QVector<Overlay::OverlayType> notifications;
};

class BlockingOverlayRenderer final : public Overlay::IOverlayRenderer
{
public:
    explicit BlockingOverlayRenderer(Overlay::OverlayManager* manager) :
        m_Manager(manager)
    {
    }

    void notifyOverlayUpdated(Overlay::OverlayType type) override
    {
        const int callIndex = m_CallCount.fetch_add(1) + 1;
        const int active = m_ActiveCallbacks.fetch_add(1) + 1;
        int maximum = m_MaxActiveCallbacks.load();
        while (active > maximum &&
               !m_MaxActiveCallbacks.compare_exchange_weak(maximum, active)) {
        }

        if (callIndex == 1) {
            firstCallbackEntered.release();
            releaseFirstCallback.acquire();
        }
        else {
            secondCallbackEntered.release();
        }

        SDL_Surface* surface = nullptr;
        Overlay::OverlayPresentation presentation;
        if (m_Manager->getUpdatedOverlaySurface(type, &surface, &presentation)) {
            {
                std::lock_guard<std::mutex> lock(m_ConsumedMutex);
                m_ConsumedMargins.push_back(presentation.marginPx);
            }
            SDL_FreeSurface(surface);
        }

        m_ActiveCallbacks.fetch_sub(1);
    }

    int maxActiveCallbacks() const
    {
        return m_MaxActiveCallbacks.load();
    }

    int callbackCount() const
    {
        return m_CallCount.load();
    }

    QVector<int> consumedMargins() const
    {
        std::lock_guard<std::mutex> lock(m_ConsumedMutex);
        return m_ConsumedMargins;
    }

    QSemaphore firstCallbackEntered;
    QSemaphore secondCallbackEntered;
    QSemaphore releaseFirstCallback;

private:
    Overlay::OverlayManager* m_Manager;
    std::atomic<int> m_CallCount {0};
    std::atomic<int> m_ActiveCallbacks {0};
    std::atomic<int> m_MaxActiveCallbacks {0};
    mutable std::mutex m_ConsumedMutex;
    QVector<int> m_ConsumedMargins;
};

class OverlayLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void placesAndConstrainsOverlays_data();
    void placesAndConstrainsOverlays();
    void layoutStateRecomputesAfterResizeWithoutSurfaceUpdate();
    void singleOverlayArbitratesStatusOverRetainedDeck();
    void deletesManagerOwnedSurfacesExactlyOnce();
    void permitsSurfaceDeleterToReenterPublication();
    void serializesNotificationsPerOverlayType();
    void rendererDetachWaitsForInFlightCallback();
    void composesWideToNarrowOverlayWithinHalfOpenBounds();
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

void OverlayLayoutTest::layoutStateRecomputesAfterResizeWithoutSurfaceUpdate()
{
    Overlay::OverlayLayoutState state;
    state.setSurface(
        640,
        360,
        {Overlay::OverlayAnchor::TopCenter, 0, 0.5f, 1.0f});

    SDL_FRect rect;
    QVERIFY(state.updateLayout(1920, 1080, false, &rect));
    QCOMPARE(rect.x, 640.0f);
    QCOMPARE(rect.y, 0.0f);
    QCOMPARE(rect.w, 640.0f);
    QCOMPARE(rect.h, 360.0f);

    QVERIFY(!state.updateLayout(1920, 1080, false, &rect));

    QVERIFY(state.updateLayout(800, 600, false, &rect));
    QCOMPARE(rect.x, 200.0f);
    QCOMPARE(rect.y, 0.0f);
    QCOMPARE(rect.w, 400.0f);
    QCOMPARE(rect.h, 225.0f);
}

void OverlayLayoutTest::singleOverlayArbitratesStatusOverRetainedDeck()
{
    Overlay::SingleOverlayArbiter arbiter;
    const Overlay::OverlayPresentation deckPresentation {
        Overlay::OverlayAnchor::TopCenter, 0, 1.0f, 1.0f};
    const Overlay::OverlayPresentation statusPresentation {
        Overlay::OverlayAnchor::BottomLeft, 0, 1.0f, 1.0f};

    SDL_Surface* deck = SDL_CreateRGBSurfaceWithFormat(
        0, 64, 36, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface* status = SDL_CreateRGBSurfaceWithFormat(
        0, 80, 24, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(deck != nullptr);
    QVERIFY(status != nullptr);

    QVERIFY(arbiter.update(
        Overlay::OverlayDeck, deck, deckPresentation, true));

    Overlay::OverlayType selectedType = Overlay::OverlayMax;
    SDL_Surface* selectedSurface = nullptr;
    Overlay::OverlayPresentation selectedPresentation;
    QVERIFY(arbiter.getSelection(
        &selectedType, &selectedSurface, &selectedPresentation));
    QCOMPARE(selectedType, Overlay::OverlayDeck);
    QCOMPARE(selectedSurface, deck);
    QCOMPARE(selectedPresentation.anchor, Overlay::OverlayAnchor::TopCenter);

    QVERIFY(arbiter.update(
        Overlay::OverlayStatusUpdate, status, statusPresentation, true));
    QVERIFY(arbiter.getSelection(
        &selectedType, &selectedSurface, &selectedPresentation));
    QCOMPARE(selectedType, Overlay::OverlayStatusUpdate);
    QCOMPARE(selectedSurface, status);

    QVERIFY(arbiter.update(
        Overlay::OverlayStatusUpdate, nullptr, statusPresentation, false));
    QVERIFY(arbiter.getSelection(
        &selectedType, &selectedSurface, &selectedPresentation));
    QCOMPARE(selectedType, Overlay::OverlayDeck);
    QCOMPARE(selectedSurface, deck);

    SDL_Surface* secondStatus = SDL_CreateRGBSurfaceWithFormat(
        0, 96, 28, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(secondStatus != nullptr);
    QVERIFY(arbiter.update(
        Overlay::OverlayStatusUpdate, secondStatus, statusPresentation, true));

    QVERIFY(!arbiter.update(
        Overlay::OverlayDeck, nullptr, deckPresentation, false));
    QVERIFY(arbiter.getSelection(
        &selectedType, &selectedSurface, &selectedPresentation));
    QCOMPARE(selectedType, Overlay::OverlayStatusUpdate);
    QCOMPARE(selectedSurface, secondStatus);

    QVERIFY(arbiter.update(
        Overlay::OverlayStatusUpdate, nullptr, statusPresentation, false));
    QVERIFY(!arbiter.getSelection(
        &selectedType, &selectedSurface, &selectedPresentation));

    SDL_Surface* finalDeck = SDL_CreateRGBSurfaceWithFormat(
        0, 48, 27, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(finalDeck != nullptr);
    QVERIFY(arbiter.update(
        Overlay::OverlayDeck, finalDeck, deckPresentation, true));
    QVERIFY(arbiter.update(
        Overlay::OverlayDeck, nullptr, deckPresentation, false));
    QVERIFY(!arbiter.getSelection(
        &selectedType, &selectedSurface, &selectedPresentation));
}

void OverlayLayoutTest::deletesManagerOwnedSurfacesExactlyOnce()
{
    QVector<SDL_Surface*> deleted;
    int nullDeletes = 0;
    const Overlay::OverlaySurfaceDeleter deleter =
        [&deleted, &nullDeletes](SDL_Surface* surface) {
            if (surface == nullptr) {
                nullDeletes++;
            }
            else {
                deleted.push_back(surface);
            }
        };

    SDL_Surface* superseded = reinterpret_cast<SDL_Surface*>(quintptr(0x1000));
    SDL_Surface* cleared = reinterpret_cast<SDL_Surface*>(quintptr(0x2000));
    SDL_Surface* pendingAtDestruction = reinterpret_cast<SDL_Surface*>(quintptr(0x3000));
    SDL_Surface* consumed = nullptr;
    SDL_Surface* transferred = reinterpret_cast<SDL_Surface*>(quintptr(0x4000));

    {
        Overlay::OverlayManager manager(deleter);
        manager.updateOverlaySurface(
            Overlay::OverlayDeck,
            superseded,
            {Overlay::OverlayAnchor::TopCenter, 1, 1.0f, 1.0f});
        manager.updateOverlaySurface(
            Overlay::OverlayDeck,
            cleared,
            {Overlay::OverlayAnchor::TopCenter, 2, 1.0f, 1.0f});
        QCOMPARE(deleted.count(superseded), 1);

        manager.updateOverlaySurface(
            Overlay::OverlayDeck,
            nullptr,
            {Overlay::OverlayAnchor::TopCenter, 3, 1.0f, 1.0f});
        QCOMPARE(deleted.count(cleared), 1);
        QCOMPARE(nullDeletes, 0);

        manager.updateOverlaySurface(
            Overlay::OverlayStatusUpdate,
            pendingAtDestruction,
            {Overlay::OverlayAnchor::BottomLeft, 4, 1.0f, 1.0f});
        manager.updateOverlaySurface(
            Overlay::OverlayDebug,
            transferred,
            {Overlay::OverlayAnchor::TopLeft, 5, 1.0f, 1.0f});

        Overlay::OverlayPresentation presentation;
        QVERIFY(manager.getUpdatedOverlaySurface(
            Overlay::OverlayDebug, &consumed, &presentation));
        QCOMPARE(consumed, transferred);
        QCOMPARE(presentation.marginPx, 5);
    }

    QCOMPARE(deleted.count(pendingAtDestruction), 1);
    QCOMPARE(deleted.count(transferred), 0);
    QCOMPARE(nullDeletes, 0);

    deleter(consumed);
    QCOMPARE(deleted.count(transferred), 1);
}

void OverlayLayoutTest::permitsSurfaceDeleterToReenterPublication()
{
#ifndef Q_OS_UNIX
    QSKIP("Bounded deadlock regression requires fork()/waitpid()");
#else
    const pid_t child = fork();
    QVERIFY2(child >= 0, "fork() failed");

    if (child == 0) {
        Overlay::OverlayManager* managerPtr = nullptr;
        bool reentered = false;
        SDL_Surface* superseded = reinterpret_cast<SDL_Surface*>(quintptr(0x1000));
        SDL_Surface* replacement = reinterpret_cast<SDL_Surface*>(quintptr(0x2000));
        const Overlay::OverlayPresentation presentation {
            Overlay::OverlayAnchor::TopCenter, 0, 1.0f, 1.0f};

        Overlay::OverlayManager manager(
            [&](SDL_Surface* surface) {
                if (surface == superseded) {
                    reentered = true;
                    managerPtr->updateOverlaySurface(
                        Overlay::OverlayDeck, nullptr, presentation);
                }
            });
        managerPtr = &manager;
        manager.updateOverlaySurface(
            Overlay::OverlayDeck, superseded, presentation);
        manager.updateOverlaySurface(
            Overlay::OverlayDeck, replacement, presentation);
        _exit(reentered ? 0 : 2);
    }

    int status = 0;
    bool exited = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1000) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            exited = true;
            break;
        }
        QVERIFY2(result >= 0, "waitpid() failed");
        QTest::qWait(10);
    }

    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }

    QVERIFY2(exited, "surface deleter deadlocked while re-entering same-type publication");
    QVERIFY2(WIFEXITED(status), "surface-deleter child did not exit normally");
    QCOMPARE(WEXITSTATUS(status), 0);
#endif
}

void OverlayLayoutTest::serializesNotificationsPerOverlayType()
{
    {
        Overlay::OverlayManager manager;
        BlockingOverlayRenderer renderer(&manager);
        manager.setOverlayRenderer(&renderer);

        SDL_Surface* firstSurface = SDL_CreateRGBSurfaceWithFormat(
            0, 16, 9, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_Surface* secondSurface = SDL_CreateRGBSurfaceWithFormat(
            0, 32, 18, 32, SDL_PIXELFORMAT_ARGB8888);
        QVERIFY(firstSurface != nullptr);
        QVERIFY(secondSurface != nullptr);

        std::thread firstProducer([&]() {
            manager.updateOverlaySurface(
                Overlay::OverlayDeck,
                firstSurface,
                {Overlay::OverlayAnchor::TopCenter, 1, 1.0f, 1.0f});
        });
        QVERIFY(renderer.firstCallbackEntered.tryAcquire(1, 1000));

        std::thread secondProducer([&]() {
            manager.updateOverlaySurface(
                Overlay::OverlayDeck,
                secondSurface,
                {Overlay::OverlayAnchor::TopCenter, 2, 1.0f, 1.0f});
        });
        const bool sameTypeCallbacksOverlapped =
            renderer.secondCallbackEntered.tryAcquire(1, 250);

        renderer.releaseFirstCallback.release();
        firstProducer.join();
        secondProducer.join();

        QVERIFY(!sameTypeCallbacksOverlapped);
        QCOMPARE(renderer.maxActiveCallbacks(), 1);
        QCOMPARE(renderer.consumedMargins(), QVector<int>({1, 2}));
    }

    {
        Overlay::OverlayManager manager;
        manager.setOverlayState(Overlay::OverlayDeck, true);
        BlockingOverlayRenderer renderer(&manager);
        manager.setOverlayRenderer(&renderer);

        SDL_Surface* deckSurface = SDL_CreateRGBSurfaceWithFormat(
            0, 16, 9, 32, SDL_PIXELFORMAT_ARGB8888);
        QVERIFY(deckSurface != nullptr);

        std::thread deckProducer([&]() {
            manager.updateOverlaySurface(
                Overlay::OverlayDeck,
                deckSurface,
                {Overlay::OverlayAnchor::TopCenter, 5, 1.0f, 1.0f});
        });
        QVERIFY(renderer.firstCallbackEntered.tryAcquire(1, 1000));

        std::thread clearProducer([&]() {
            manager.setOverlayState(Overlay::OverlayDeck, false);
        });
        const bool clearCallbackOverlapped =
            renderer.secondCallbackEntered.tryAcquire(1, 250);

        renderer.releaseFirstCallback.release();
        deckProducer.join();
        clearProducer.join();

        QVERIFY(!clearCallbackOverlapped);
        QCOMPARE(renderer.maxActiveCallbacks(), 1);
        QCOMPARE(renderer.consumedMargins(), QVector<int>({5, 0}));
        QVERIFY(!manager.isOverlayEnabled(Overlay::OverlayDeck));
    }

    {
        Overlay::OverlayManager manager;
        BlockingOverlayRenderer renderer(&manager);
        manager.setOverlayRenderer(&renderer);

        SDL_Surface* deckSurface = SDL_CreateRGBSurfaceWithFormat(
            0, 16, 9, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_Surface* statusSurface = SDL_CreateRGBSurfaceWithFormat(
            0, 24, 12, 32, SDL_PIXELFORMAT_ARGB8888);
        QVERIFY(deckSurface != nullptr);
        QVERIFY(statusSurface != nullptr);

        std::thread deckProducer([&]() {
            manager.updateOverlaySurface(
                Overlay::OverlayDeck,
                deckSurface,
                {Overlay::OverlayAnchor::TopCenter, 3, 1.0f, 1.0f});
        });
        QVERIFY(renderer.firstCallbackEntered.tryAcquire(1, 1000));

        std::thread statusProducer([&]() {
            manager.updateOverlaySurface(
                Overlay::OverlayStatusUpdate,
                statusSurface,
                {Overlay::OverlayAnchor::BottomLeft, 4, 1.0f, 1.0f});
        });
        const bool differentTypeCallbacksOverlapped =
            renderer.secondCallbackEntered.tryAcquire(1, 1000);

        renderer.releaseFirstCallback.release();
        deckProducer.join();
        statusProducer.join();

        QVERIFY(differentTypeCallbacksOverlapped);
        QCOMPARE(renderer.maxActiveCallbacks(), 2);
    }
}

void OverlayLayoutTest::rendererDetachWaitsForInFlightCallback()
{
    Overlay::OverlayManager manager;
    BlockingOverlayRenderer renderer(&manager);
    manager.setOverlayRenderer(&renderer);

    SDL_Surface* firstSurface = SDL_CreateRGBSurfaceWithFormat(
        0, 16, 9, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(firstSurface != nullptr);

    std::thread producer([&]() {
        manager.updateOverlaySurface(
            Overlay::OverlayDeck,
            firstSurface,
            {Overlay::OverlayAnchor::TopCenter, 1, 1.0f, 1.0f});
    });
    const bool callbackEntered =
        renderer.firstCallbackEntered.tryAcquire(1, 1000);

    QSemaphore detachReturned;
    std::thread detacher([&]() {
        manager.setOverlayRenderer(nullptr);
        detachReturned.release();
    });
    const bool detachedWhileCallbackBlocked =
        detachReturned.tryAcquire(1, 250);

    renderer.releaseFirstCallback.release();
    producer.join();
    const bool detachedAfterCallback = detachedWhileCallbackBlocked ||
        detachReturned.tryAcquire(1, 1000);
    detacher.join();

    QVERIFY(callbackEntered);
    QVERIFY2(!detachedWhileCallbackBlocked,
             "renderer detach returned before the in-flight callback completed");
    QVERIFY(detachedAfterCallback);
    QCOMPARE(renderer.callbackCount(), 1);

    SDL_Surface* afterDetach = SDL_CreateRGBSurfaceWithFormat(
        0, 32, 18, 32, SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(afterDetach != nullptr);
    manager.updateOverlaySurface(
        Overlay::OverlayDeck,
        afterDetach,
        {Overlay::OverlayAnchor::TopCenter, 2, 1.0f, 1.0f});

    QCOMPARE(renderer.callbackCount(), 1);
    QVERIFY(!renderer.secondCallbackEntered.tryAcquire(1, 0));
}

void OverlayLayoutTest::composesWideToNarrowOverlayWithinHalfOpenBounds()
{
    constexpr int displayWidth = 1920;
    constexpr int displayHeight = 4;
    constexpr int guardWidth = 640;
    constexpr int sourceWidth = 640;
    constexpr int sourceHeight = 2;
    constexpr quint32 guardPixel = 0xCCCCCCCC;
    constexpr quint32 sourcePixel = 0x80402010;
    constexpr quint32 sourceGuardPixel = 0xDEADBEEF;

    QVector<quint32> destinationPixels(
        (displayWidth + guardWidth) * displayHeight, guardPixel);
    QVector<quint32> sourcePixels(
        sourceWidth * (sourceHeight + 1), sourceGuardPixel);
    std::fill_n(sourcePixels.begin(), sourceWidth * sourceHeight, sourcePixel);

    SDL_Surface* destination = SDL_CreateRGBSurfaceWithFormatFrom(
        destinationPixels.data(),
        displayWidth,
        displayHeight,
        32,
        (displayWidth + guardWidth) * static_cast<int>(sizeof(quint32)),
        SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface* source = SDL_CreateRGBSurfaceWithFormatFrom(
        sourcePixels.data(),
        sourceWidth,
        sourceHeight,
        32,
        sourceWidth * static_cast<int>(sizeof(quint32)),
        SDL_PIXELFORMAT_ARGB8888);
    QVERIFY(destination != nullptr);
    QVERIFY(source != nullptr);

    const SDL_Rect previousRect {0, 0, displayWidth, displayHeight};
    const SDL_Rect newRect {640, 1, sourceWidth, sourceHeight};
    SDL_Rect damagedRect {};
    const bool composed = Overlay::composeOverlaySurfacePatch(
        destination, source, previousRect, newRect, &damagedRect);

    QVERIFY(composed);
    QCOMPARE(damagedRect.x, 0);
    QCOMPARE(damagedRect.y, 0);
    QCOMPARE(damagedRect.w, displayWidth);
    QCOMPARE(damagedRect.h, displayHeight);

    for (int y = 0; y < displayHeight; y++) {
        const quint32* row = destinationPixels.constData() +
            y * (displayWidth + guardWidth);
        for (int x = 0; x < displayWidth; x++) {
            const bool inReplacement =
                y >= newRect.y && y < newRect.y + newRect.h &&
                x >= newRect.x && x < newRect.x + newRect.w;
            QCOMPARE(row[x], inReplacement ? sourcePixel : quint32(0));
        }
        for (int x = displayWidth; x < displayWidth + guardWidth; x++) {
            QCOMPARE(row[x], guardPixel);
        }
    }

    SDL_FreeSurface(source);
    SDL_FreeSurface(destination);
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
