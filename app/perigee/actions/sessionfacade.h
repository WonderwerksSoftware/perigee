#pragma once

#include "actiontypes.h"

#include <QObject>

#include <functional>

class SessionFacade : public QObject
{
public:
    using PhysicalDisplayCompletion = std::function<void(const ActionResult&)>;

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

    virtual int physicalDisplayCount() const = 0;
    virtual int lastRequestedPhysicalDisplay() const = 0;
    virtual bool physicalDisplaySwitchActive() const = 0;
    virtual bool requestPhysicalDisplay(
        int displayNumber, PhysicalDisplayCompletion completion) = 0;

    virtual bool requestClientDisconnect() = 0;
    virtual bool requestPerigeeQuit() = 0;
};
