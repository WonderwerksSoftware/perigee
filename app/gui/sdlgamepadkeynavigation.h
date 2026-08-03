#pragma once

#include <QTimer>
#include <QEvent>

#include "SDL_compat.h"

#include "perigee/input/deckbindings.h"
#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    Q_INVOKABLE int getConnectedGamepads();

    Q_INVOKABLE void beginControllerBindingCapture();

    Q_INVOKABLE void cancelControllerBindingCapture();

signals:
    void controllerBindingCaptured(int buttons);
    void controllerBindingCaptureCancelled();

private:
    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void handleControllerBindingButton(SDL_JoystickID controller,
                                       Uint8 button, bool pressed);

    void updateTimerState();

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    DeckControllerChordCapture m_DeckBindingCapture;
    SDL_JoystickID m_DeckBindingCaptureController = -1;
};
