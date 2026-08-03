#include "test_registry.h"

#include "perigee/deck/decksurfacerenderer.h"

#include <QImage>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtTest>

#include <SDL.h>

#include <cstring>
#include <memory>

class DeckSurfaceRendererTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void rendersPremultipliedArgbWithTransparencyInQmlCoordinates();
    void reportsComponentLoadFailure();
};

void DeckSurfaceRendererTest::initTestCase()
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
}

void DeckSurfaceRendererTest::rendersPremultipliedArgbWithTransparencyInQmlCoordinates()
{
    QQmlEngine engine;
    QString error;
    QImage image;

    {
        DeckSurfaceRenderer renderer;
        QVERIFY2(renderer.initialize(
                     &engine,
                     QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeckProbe.qml")),
                     &error),
                 qPrintable(error));
        renderer.resize(QSize(64, 48), 1.0);

        QVERIFY2(renderer.render(&image, &error), qPrintable(error));
    }

    // The image must remain valid after all temporary readback, FBO, and QML
    // renderer storage has been destroyed.
    QCOMPARE(image.size(), QSize(64, 48));
    QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);

    const QColor center = image.pixelColor(32, 24);
    QCOMPARE(center, QColor(255, 0, 255, 255));
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);

    // The probe is intentionally off-center vertically. These checks catch a
    // missing readback flip and an accidental second flip independently of the
    // required center/corner assertions above.
    QCOMPARE(image.pixelColor(32, 10), QColor(255, 0, 255, 255));
    QCOMPARE(image.pixelColor(32, 38).alpha(), 0);

    quint32 packedCenter;
    std::memcpy(&packedCenter,
                image.constScanLine(24) + 32 * sizeof(quint32),
                sizeof(packedCenter));
    using SdlPixelFormat = std::unique_ptr<SDL_PixelFormat,
                                           decltype(&SDL_FreeFormat)>;
    SdlPixelFormat sdlFormat(
        SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888), SDL_FreeFormat);
    QVERIFY(sdlFormat != nullptr);
    quint8 red;
    quint8 green;
    quint8 blue;
    quint8 alpha;
    SDL_GetRGBA(packedCenter,
                sdlFormat.get(),
                &red,
                &green,
                &blue,
                &alpha);
    QCOMPARE(red, quint8(255));
    QCOMPARE(green, quint8(0));
    QCOMPARE(blue, quint8(255));
    QCOMPARE(alpha, quint8(255));
}

void DeckSurfaceRendererTest::reportsComponentLoadFailure()
{
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;

    QVERIFY(!renderer.initialize(
        &engine,
        QUrl(QStringLiteral("qrc:/gui/perigee/DoesNotExist.qml")),
        &error));
    QVERIFY2(error.contains(QStringLiteral("component"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("DoesNotExist.qml")),
             qPrintable(error));
}

REGISTER_PERIGEE_TEST(DeckSurfaceRendererTest);

#include "test_decksurfacerenderer.moc"
