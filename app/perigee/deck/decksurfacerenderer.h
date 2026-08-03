#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QUrl>

#include <memory>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QObject;
class QQmlEngine;
class DeckController;

namespace Overlay {
class OverlayManager;
}

class DeckSurfaceRenderer final
{
public:
    DeckSurfaceRenderer();
    ~DeckSurfaceRenderer();

    DeckSurfaceRenderer(const DeckSurfaceRenderer&) = delete;
    DeckSurfaceRenderer& operator=(const DeckSurfaceRenderer&) = delete;

    bool initialize(QQmlEngine* engine,
                    const QUrl& componentUrl,
                    QString* error);
    bool initialize(QQmlEngine* engine,
                    const QUrl& componentUrl,
                    DeckController* controller,
                    QString* error);
    void resize(QSize logicalSize, qreal devicePixelRatio);
    bool render(QImage* premultipliedArgb, QString* error);
    bool renderAndPublishDeck(Overlay::OverlayManager* overlayManager,
                              QString* error);
    bool sendKeyEvent(QKeyEvent* event);
    bool sendPointerEvent(QMouseEvent* event);
    bool sendWheelEvent(QWheelEvent* event);
    bool sendTextInput(const QString& text);
    bool isDirty() const;
    void markDirty();
    QObject* rootObject() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};
