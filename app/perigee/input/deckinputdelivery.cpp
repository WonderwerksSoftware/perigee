#include "deckinputdelivery.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

bool DeckInputDelivery::handles(DeckInputRouter::Action action)
{
    using Action = DeckInputRouter::Action;
    switch (action) {
    case Action::Key:
    case Action::TextInput:
    case Action::NavigateUp:
    case Action::NavigateDown:
    case Action::NavigateLeft:
    case Action::NavigateRight:
    case Action::Activate:
    case Action::Back:
    case Action::PointerMove:
    case Action::PointerPress:
    case Action::PointerRelease:
    case Action::PointerWheel:
        return true;
    default:
        return false;
    }
}

void DeckInputDelivery::deliver(const DeckInputRouter::Result& result,
                                DeckInputSink& sink)
{
    using Action = DeckInputRouter::Action;
    const auto sendNavigationKey = [&sink](int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        sink.sendKeyEvent(&press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        sink.sendKeyEvent(&release);
    };

    switch (result.action) {
    case Action::Key: {
        QKeyEvent event(result.pressed ? QEvent::KeyPress : QEvent::KeyRelease,
                        result.key, result.keyModifiers, result.text,
                        result.autoRepeat);
        sink.sendKeyEvent(&event);
        break;
    }
    case Action::TextInput:
        sink.sendTextInput(result.text);
        break;
    case Action::NavigateUp:
        sendNavigationKey(Qt::Key_Up);
        break;
    case Action::NavigateDown:
        sendNavigationKey(Qt::Key_Down);
        break;
    case Action::NavigateLeft:
        sendNavigationKey(Qt::Key_Left);
        break;
    case Action::NavigateRight:
        sendNavigationKey(Qt::Key_Right);
        break;
    case Action::Activate:
        sendNavigationKey(Qt::Key_Return);
        break;
    case Action::Back:
        sendNavigationKey(Qt::Key_Escape);
        break;
    case Action::PointerMove:
    case Action::PointerPress:
    case Action::PointerRelease: {
        QEvent::Type type = QEvent::MouseMove;
        if (result.action == Action::PointerPress) {
            type = QEvent::MouseButtonPress;
        }
        else if (result.action == Action::PointerRelease) {
            type = QEvent::MouseButtonRelease;
        }
        QMouseEvent event(type, result.position, result.position,
                          result.position, result.mouseButton,
                          result.mouseButtons, Qt::NoModifier);
        sink.sendPointerEvent(&event);
        break;
    }
    case Action::PointerWheel: {
        QWheelEvent event(result.position, result.position, QPoint(),
                          result.wheelDelta, result.mouseButtons,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        sink.sendWheelEvent(&event);
        break;
    }
    default:
        break;
    }
}
