#pragma once

#include "perigee/input/deckinputdelivery.h"

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

class DeckSurfaceRenderer final : public DeckInputSink
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
    bool sendKeyEvent(QKeyEvent* event) override;
    bool sendPointerEvent(QMouseEvent* event) override;
    bool sendWheelEvent(QWheelEvent* event) override;
    bool sendTextInput(const QString& text) override;
    bool isDirty() const;
    void markDirty();
    QObject* rootObject() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};
