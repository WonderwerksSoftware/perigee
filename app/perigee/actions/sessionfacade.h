#pragma once

#include <QObject>

class SessionFacade : public QObject
{
public:
    explicit SessionFacade(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    virtual ~SessionFacade() = default;

    virtual bool statsOverlayEnabled() const = 0;
    virtual bool mouseCaptureEnabled() const = 0;
    virtual bool keyboardCaptureEnabled() const = 0;
    virtual bool fullscreenEnabled() const = 0;

    virtual bool setStatsOverlayEnabled(bool enabled) = 0;
    virtual bool setMouseCaptureEnabled(bool enabled) = 0;
    virtual bool setKeyboardCaptureEnabled(bool enabled) = 0;
    virtual bool setFullscreenEnabled(bool enabled) = 0;

    virtual bool requestClientDisconnect() = 0;
    virtual bool requestPerigeeQuit() = 0;
};
