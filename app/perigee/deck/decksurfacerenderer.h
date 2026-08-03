#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QUrl>

#include <memory>

class QKeyEvent;
class QMouseEvent;
class QQmlEngine;

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
    void resize(QSize logicalSize, qreal devicePixelRatio);
    bool render(QImage* premultipliedArgb, QString* error);
    bool sendKeyEvent(QKeyEvent* event);
    bool sendPointerEvent(QMouseEvent* event);

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};
