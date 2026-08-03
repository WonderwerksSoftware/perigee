#pragma once

class QObject;

class SessionFacade
{
public:
    virtual ~SessionFacade() = default;

    // This object must share the facade lifetime. GameStreamAdapter observes it
    // without extending the streaming session's authority.
    virtual QObject* lifetimeAuthority() = 0;

    virtual bool statsOverlayEnabled() const = 0;
    virtual bool mouseCaptureEnabled() const = 0;
    virtual bool keyboardCaptureEnabled() const = 0;
    virtual bool fullscreenEnabled() const = 0;

    virtual bool setStatsOverlayEnabled(bool enabled) = 0;
    virtual bool setMouseCaptureEnabled(bool enabled) = 0;
    virtual bool setKeyboardCaptureEnabled(bool enabled) = 0;
    virtual bool setFullscreenEnabled(bool enabled) = 0;

    virtual void requestClientDisconnect() = 0;
    virtual void requestPerigeeQuit() = 0;
};
