#include "decksurfacerenderer.h"

#include "deckcontroller.h"
#include "streaming/video/overlaymanager.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QInputMethodEvent>
#include <QWheelEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QQuickGraphicsDevice>
#include <QQuickRenderTarget>
#endif
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QThread>

#include <SDL.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

QString componentErrors(const QQmlComponent& component)
{
    QStringList messages;
    const QList<QQmlError> errors = component.errors();
    messages.reserve(errors.size());
    for (const QQmlError& error : errors) {
        messages.push_back(error.toString());
    }
    return messages.join(QStringLiteral("; "));
}

}

class DeckSurfaceRenderer::Impl final
{
public:
    Impl() : ownerThread(QThread::currentThread())
    {
    }

    ~Impl()
    {
        releaseResources();
    }

    bool isOnOwnerThread(QString* error, const QString& operation) const
    {
        if (QThread::currentThread() != ownerThread ||
                (QCoreApplication::instance() != nullptr &&
                 QThread::currentThread() != QCoreApplication::instance()->thread())) {
            setError(error,
                     QStringLiteral("Deck renderer %1 must run on its GUI/SDL owning thread")
                         .arg(operation));
            return false;
        }
        return true;
    }

    void releaseResources()
    {
        // Qt Quick scenegraph and FBO resources are context- and thread-bound.
        // Keep the context current and destroy them in dependency order.
        const bool onOwnerThread = QThread::currentThread() == ownerThread;
        Q_ASSERT(onOwnerThread);
        if (!onOwnerThread) {
            return;
        }

        const bool current = context != nullptr && offscreenSurface != nullptr &&
            context->makeCurrent(offscreenSurface.get());
        if (quickWindow != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            quickWindow->setRenderTarget(QQuickRenderTarget());
#else
            quickWindow->setRenderTarget(nullptr);
#endif
        }
        quickWindow.reset();
        renderControl.reset();
        framebuffer.reset();
        if (current) {
            context->doneCurrent();
        }
        offscreenSurface.reset();
        context.reset();
        rootItem = nullptr;
        initialized = false;
    }

    bool createFramebuffer(QString* error)
    {
        if (!pixelSize.isValid() || pixelSize.isEmpty()) {
            setError(error, QStringLiteral("Deck render surface size is empty"));
            return false;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        quickWindow->setRenderTarget(QQuickRenderTarget());
#else
        quickWindow->setRenderTarget(nullptr);
#endif
        framebuffer.reset();

        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        format.setInternalTextureFormat(GL_RGBA8);
#else
        format.setInternalTextureFormat(GL_RGBA);
#endif
        auto candidate = std::make_unique<QOpenGLFramebufferObject>(pixelSize, format);
        if (!candidate->isValid() || !candidate->bind()) {
            setError(error,
                     QStringLiteral("Deck framebuffer is incomplete for %1x%2 pixels")
                         .arg(pixelSize.width())
                         .arg(pixelSize.height()));
            return false;
        }

        framebuffer = std::move(candidate);
        assignFramebufferRenderTarget();
        return true;
    }

    void assignFramebufferRenderTarget()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QQuickRenderTarget target =
            QQuickRenderTarget::fromOpenGLTexture(
                framebuffer->texture(), GL_RGBA8, pixelSize);
        target.setDevicePixelRatio(devicePixelRatio);
        quickWindow->setRenderTarget(target);
#else
        quickWindow->setRenderTarget(framebuffer.get());
#endif
        renderTargetDevicePixelRatio = devicePixelRatio;
    }

    QThread* ownerThread;
    std::unique_ptr<QOpenGLContext> context;
    std::unique_ptr<QOffscreenSurface> offscreenSurface;
    std::unique_ptr<QQuickRenderControl> renderControl;
    std::unique_ptr<QQuickWindow> quickWindow;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer;
    QQuickItem* rootItem = nullptr;
    QSize logicalSize;
    QSize pixelSize;
    qreal devicePixelRatio = 1.0;
    qreal renderTargetDevicePixelRatio = 0.0;
    bool initialized = false;
    bool dirty = true;
};

DeckSurfaceRenderer::DeckSurfaceRenderer() : m_Impl(std::make_unique<Impl>())
{
}

DeckSurfaceRenderer::~DeckSurfaceRenderer() = default;

bool DeckSurfaceRenderer::initialize(QQmlEngine* engine,
                                     const QUrl& componentUrl,
                                     QString* error)
{
    return initialize(engine, componentUrl, nullptr, error);
}

bool DeckSurfaceRenderer::initialize(QQmlEngine* engine,
                                     const QUrl& componentUrl,
                                     DeckController* controller,
                                     QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_Impl->isOnOwnerThread(error, QStringLiteral("initialization"))) {
        return false;
    }
    if (m_Impl->initialized) {
        setError(error, QStringLiteral("Deck renderer is already initialized"));
        return false;
    }
    if (engine == nullptr) {
        setError(error, QStringLiteral("Deck component requires an existing QML engine"));
        return false;
    }
    if (!componentUrl.isValid() || componentUrl.isEmpty()) {
        setError(error, QStringLiteral("Deck component URL is empty or invalid"));
        return false;
    }

    QQmlComponent component(
        engine, componentUrl, QQmlComponent::PreferSynchronous);
    if (!component.isReady()) {
        QString details = componentErrors(component);
        if (details.isEmpty()) {
            details = QStringLiteral("component did not become ready synchronously");
        }
        setError(error,
                 QStringLiteral("Deck component load failed for %1: %2")
                     .arg(componentUrl.toString(), details));
        return false;
    }

    std::unique_ptr<QObject> rootObject;
    if (controller != nullptr) {
        rootObject.reset(component.createWithInitialProperties({
            { QStringLiteral("deckController"),
              QVariant::fromValue(controller) },
        }));
    }
    else {
        rootObject.reset(component.create());
    }
    if (rootObject == nullptr || component.isError()) {
        QString details = componentErrors(component);
        if (details.isEmpty()) {
            details = QStringLiteral("component creation returned no object");
        }
        setError(error,
                 QStringLiteral("Deck component creation failed for %1: %2")
                     .arg(componentUrl.toString(), details));
        return false;
    }
    QQuickItem* rootItem = qobject_cast<QQuickItem*>(rootObject.get());
    if (rootItem == nullptr) {
        setError(error,
                 QStringLiteral("Deck component root is not a QQuickItem: %1")
                     .arg(componentUrl.toString()));
        return false;
    }

    auto context = std::make_unique<QOpenGLContext>();
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setDepthBufferSize(qMax(format.depthBufferSize(), 24));
    format.setStencilBufferSize(qMax(format.stencilBufferSize(), 8));
    format.setAlphaBufferSize(qMax(format.alphaBufferSize(), 8));
    context->setFormat(format);
    context->setShareContext(QOpenGLContext::globalShareContext());
    if (!context->create()) {
        setError(error, QStringLiteral("Deck OpenGL context creation failed"));
        return false;
    }

    auto offscreenSurface = std::make_unique<QOffscreenSurface>();
    offscreenSurface->setFormat(context->format());
    offscreenSurface->create();
    if (!offscreenSurface->isValid()) {
        setError(error, QStringLiteral("Deck offscreen surface creation failed"));
        return false;
    }
    if (!context->makeCurrent(offscreenSurface.get())) {
        setError(error,
                 QStringLiteral("Deck OpenGL context could not be made current"));
        return false;
    }

    auto renderControl = std::make_unique<QQuickRenderControl>();
    auto quickWindow = std::make_unique<QQuickWindow>(renderControl.get());
    quickWindow->setColor(Qt::transparent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    quickWindow->setGraphicsDevice(
        QQuickGraphicsDevice::fromOpenGLContext(context.get()));
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const bool renderControlInitialized = renderControl->initialize();
#else
    renderControl->initialize(context.get());
    const bool renderControlInitialized = true;
#endif
    if (!renderControlInitialized) {
        quickWindow.reset();
        renderControl.reset();
        context->doneCurrent();
        setError(error,
                 QStringLiteral("Deck Qt Quick render-control initialization failed"));
        return false;
    }
    context->doneCurrent();

    rootItem->setParent(quickWindow->contentItem());
    rootItem->setParentItem(quickWindow->contentItem());
    rootItem->forceActiveFocus();
    rootObject.release();

    m_Impl->context = std::move(context);
    m_Impl->offscreenSurface = std::move(offscreenSurface);
    m_Impl->renderControl = std::move(renderControl);
    m_Impl->quickWindow = std::move(quickWindow);
    m_Impl->rootItem = rootItem;
    m_Impl->initialized = true;
    m_Impl->dirty = true;
    QObject::connect(m_Impl->renderControl.get(),
                     &QQuickRenderControl::renderRequested,
                     m_Impl->quickWindow.get(),
                     [impl = m_Impl.get()] { impl->dirty = true; });
    QObject::connect(m_Impl->renderControl.get(),
                     &QQuickRenderControl::sceneChanged,
                     m_Impl->quickWindow.get(),
                     [impl = m_Impl.get()] { impl->dirty = true; });
    if (controller != nullptr) {
        const auto dirty = [impl = m_Impl.get()] { impl->dirty = true; };
        QObject::connect(controller, &DeckController::openChanged,
                         m_Impl->quickWindow.get(), dirty);
        QObject::connect(controller, &DeckController::searchTextChanged,
                         m_Impl->quickWindow.get(), dirty);
        QObject::connect(controller, &DeckController::activeCategoryChanged,
                         m_Impl->quickWindow.get(), dirty);
        QObject::connect(controller, &DeckController::focusModeChanged,
                         m_Impl->quickWindow.get(), dirty);
        QObject::connect(controller, &DeckController::confirmationChanged,
                         m_Impl->quickWindow.get(), dirty);
    }
    return true;
}

void DeckSurfaceRenderer::resize(QSize logicalSize, qreal devicePixelRatio)
{
    if (!m_Impl->isOnOwnerThread(nullptr, QStringLiteral("resize"))) {
        return;
    }

    if (m_Impl->logicalSize != logicalSize ||
            m_Impl->devicePixelRatio != devicePixelRatio) {
        m_Impl->dirty = true;
    }
    m_Impl->logicalSize = logicalSize;
    m_Impl->devicePixelRatio = devicePixelRatio;
    m_Impl->pixelSize = QSize();
    if (logicalSize.isValid() && !logicalSize.isEmpty() &&
            std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0) {
        const qreal pixelWidth = logicalSize.width() * devicePixelRatio;
        const qreal pixelHeight = logicalSize.height() * devicePixelRatio;
        if (pixelWidth <= std::numeric_limits<int>::max() &&
                pixelHeight <= std::numeric_limits<int>::max()) {
            m_Impl->pixelSize = QSize(qRound(pixelWidth), qRound(pixelHeight));
        }
    }

    if (m_Impl->quickWindow != nullptr) {
        m_Impl->quickWindow->setGeometry(
            0, 0, logicalSize.width(), logicalSize.height());
    }
    if (m_Impl->rootItem != nullptr) {
        m_Impl->rootItem->setSize(logicalSize);
    }
}

bool DeckSurfaceRenderer::render(QImage* premultipliedArgb, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_Impl->isOnOwnerThread(error, QStringLiteral("rendering"))) {
        return false;
    }
    if (premultipliedArgb == nullptr) {
        setError(error, QStringLiteral("Deck render output image is null"));
        return false;
    }
    *premultipliedArgb = QImage();
    if (!m_Impl->initialized || m_Impl->context == nullptr ||
            m_Impl->offscreenSurface == nullptr ||
            m_Impl->renderControl == nullptr ||
            m_Impl->quickWindow == nullptr) {
        setError(error, QStringLiteral("Deck renderer is not initialized"));
        return false;
    }
    if (!m_Impl->pixelSize.isValid() || m_Impl->pixelSize.isEmpty()) {
        setError(error, QStringLiteral("Deck render surface size is empty"));
        return false;
    }
    if (!m_Impl->context->makeCurrent(m_Impl->offscreenSurface.get())) {
        setError(error,
                 QStringLiteral("Deck OpenGL context could not be made current for rendering"));
        return false;
    }
    if ((m_Impl->framebuffer == nullptr ||
         m_Impl->framebuffer->size() != m_Impl->pixelSize) &&
            !m_Impl->createFramebuffer(error)) {
        m_Impl->context->doneCurrent();
        return false;
    }
    if (m_Impl->renderTargetDevicePixelRatio != m_Impl->devicePixelRatio) {
        m_Impl->assignFramebufferRenderTarget();
    }

    m_Impl->renderControl->polishItems();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_Impl->renderControl->beginFrame();
#endif
    m_Impl->renderControl->sync();
    m_Impl->renderControl->render();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_Impl->renderControl->endFrame();
#endif

    // QOpenGLFramebufferObject::toImage(true) performs the sole vertical
    // mirror, converting OpenGL's bottom-left origin to QML/image coordinates.
    QImage readback = m_Impl->framebuffer->toImage(true);
    m_Impl->context->doneCurrent();
    if (readback.isNull() || readback.size() != m_Impl->pixelSize) {
        setError(error,
                 QStringLiteral("Deck framebuffer readback failed for %1x%2 pixels")
                     .arg(m_Impl->pixelSize.width())
                     .arg(m_Impl->pixelSize.height()));
        return false;
    }

    QImage converted =
        readback.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (converted.isNull() ||
            converted.format() != QImage::Format_ARGB32_Premultiplied) {
        setError(error,
                 QStringLiteral("Deck framebuffer readback could not be converted to premultiplied ARGB"));
        return false;
    }

    *premultipliedArgb = std::move(converted);
    m_Impl->dirty = false;
    return true;
}

bool DeckSurfaceRenderer::renderAndPublishDeck(
        Overlay::OverlayManager* overlayManager,
        QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (overlayManager == nullptr) {
        setError(error, QStringLiteral("Deck overlay manager is null"));
        return false;
    }

    QImage image;
    if (!render(&image, error)) {
        return false;
    }
    if (image.format() != QImage::Format_ARGB32_Premultiplied ||
            image.width() <= 0 || image.height() <= 0 ||
            image.depth() != 32) {
        setError(error,
                 QStringLiteral("Deck render output is not premultiplied 32-bit ARGB"));
        return false;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, image.width(), image.height(), 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        setError(error,
                 QStringLiteral("Deck SDL surface allocation failed: %1")
                     .arg(QString::fromLocal8Bit(SDL_GetError())));
        return false;
    }

    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 4;
    if (rowBytes <= 0 || image.bytesPerLine() < rowBytes ||
            surface->pitch < rowBytes || surface->pixels == nullptr) {
        SDL_FreeSurface(surface);
        setError(error,
                 QStringLiteral("Deck SDL surface has an invalid row pitch"));
        return false;
    }
    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0) {
        const QString lockError = QString::fromLocal8Bit(SDL_GetError());
        SDL_FreeSurface(surface);
        setError(error,
                 QStringLiteral("Deck SDL surface lock failed: %1").arg(lockError));
        return false;
    }

    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(static_cast<uchar*>(surface->pixels) +
                        static_cast<qsizetype>(y) * surface->pitch,
                    image.constScanLine(y),
                    static_cast<std::size_t>(rowBytes));
    }
    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }

    // OverlayManager takes ownership, including when no renderer is active.
    overlayManager->updateOverlaySurface(
        Overlay::OverlayDeck,
        surface,
        { Overlay::OverlayAnchor::TopCenter, 0, 1.0f, 1.0f, true });
    return true;
}

bool DeckSurfaceRenderer::sendKeyEvent(QKeyEvent* event)
{
    if (event == nullptr || !m_Impl->initialized ||
            !m_Impl->isOnOwnerThread(nullptr, QStringLiteral("key event delivery"))) {
        return false;
    }
    const bool delivered = QCoreApplication::sendEvent(m_Impl->quickWindow.get(), event);
    m_Impl->dirty = true;
    return delivered;
}

bool DeckSurfaceRenderer::sendPointerEvent(QMouseEvent* event)
{
    if (event == nullptr || !m_Impl->initialized ||
            !m_Impl->isOnOwnerThread(nullptr, QStringLiteral("pointer event delivery"))) {
        return false;
    }
    const bool delivered = QCoreApplication::sendEvent(m_Impl->quickWindow.get(), event);
    m_Impl->dirty = true;
    return delivered;
}

bool DeckSurfaceRenderer::sendWheelEvent(QWheelEvent* event)
{
    if (event == nullptr || !m_Impl->initialized ||
            !m_Impl->isOnOwnerThread(nullptr, QStringLiteral("wheel event delivery"))) {
        return false;
    }
    const bool delivered = QCoreApplication::sendEvent(m_Impl->quickWindow.get(), event);
    m_Impl->dirty = true;
    return delivered;
}

bool DeckSurfaceRenderer::sendTextInput(const QString& text)
{
    if (text.isEmpty() || !m_Impl->initialized ||
            !m_Impl->isOnOwnerThread(nullptr, QStringLiteral("text input delivery"))) {
        return false;
    }
    QInputMethodEvent event;
    event.setCommitString(text);
    const bool delivered = QCoreApplication::sendEvent(m_Impl->quickWindow.get(), &event);
    m_Impl->dirty = true;
    return delivered;
}

bool DeckSurfaceRenderer::isDirty() const
{
    return m_Impl->dirty;
}

void DeckSurfaceRenderer::markDirty()
{
    m_Impl->dirty = true;
}

QObject* DeckSurfaceRenderer::rootObject() const
{
    return m_Impl->rootItem;
}
