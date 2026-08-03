#pragma once

#include "deckinputrouter.h"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class DeckController;

class DeckInputSink
{
public:
    virtual ~DeckInputSink() = default;
    virtual bool sendKeyEvent(QKeyEvent* event) = 0;
    virtual bool sendPointerEvent(QMouseEvent* event) = 0;
    virtual bool sendWheelEvent(QWheelEvent* event) = 0;
    virtual bool sendTextInput(const QString& text) = 0;
};

class DeckInputDelivery final
{
public:
    static bool handles(DeckInputRouter::Action action);
    static void deliver(const DeckInputRouter::Result& result,
                        DeckInputSink& sink);
    static bool openDeck(const DeckInputRouter::Result& result,
                         DeckController& controller,
                         bool swapFaceButtons);
};
