#include "decksurfacerenderer.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QThread>

#include <cmath>
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
            quickWindow->setRenderTarget(QQuickRenderTarget());
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

        quickWindow->setRenderTarget(QQuickRenderTarget());
        framebuffer.reset();

        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setInternalTextureFormat(GL_RGBA8);
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
        QQuickRenderTarget target =
            QQuickRenderTarget::fromOpenGLTexture(
                framebuffer->texture(), GL_RGBA8, pixelSize);
        target.setDevicePixelRatio(devicePixelRatio);
        quickWindow->setRenderTarget(target);
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
};

DeckSurfaceRenderer::DeckSurfaceRenderer() : m_Impl(std::make_unique<Impl>())
{
}

DeckSurfaceRenderer::~DeckSurfaceRenderer() = default;

bool DeckSurfaceRenderer::initialize(QQmlEngine* engine,
                                     const QUrl& componentUrl,
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

    std::unique_ptr<QObject> rootObject(component.create());
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
    quickWindow->setGraphicsDevice(
        QQuickGraphicsDevice::fromOpenGLContext(context.get()));
    if (!renderControl->initialize()) {
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
    return true;
}

void DeckSurfaceRenderer::resize(QSize logicalSize, qreal devicePixelRatio)
{
    if (!m_Impl->isOnOwnerThread(nullptr, QStringLiteral("resize"))) {
        return;
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
    m_Impl->renderControl->beginFrame();
    m_Impl->renderControl->sync();
    m_Impl->renderControl->render();
    m_Impl->renderControl->endFrame();

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
    return true;
}

bool DeckSurfaceRenderer::sendKeyEvent(QKeyEvent* event)
{
    return event != nullptr && m_Impl->initialized &&
        m_Impl->isOnOwnerThread(nullptr, QStringLiteral("key event delivery")) &&
        QCoreApplication::sendEvent(m_Impl->quickWindow.get(), event);
}

bool DeckSurfaceRenderer::sendPointerEvent(QMouseEvent* event)
{
    return event != nullptr && m_Impl->initialized &&
        m_Impl->isOnOwnerThread(nullptr, QStringLiteral("pointer event delivery")) &&
        QCoreApplication::sendEvent(m_Impl->quickWindow.get(), event);
}
