#pragma once

#include "SDL_compat.h"

#include <QString>
#include <Qt>

#include <optional>

class QSettings;

class DeckBindings final
{
public:
    DeckBindings();
    DeckBindings(int keyModifiers, int keyScancode,
                 quint32 controllerButtons,
                 bool legacyGamepadDisconnect);

    static DeckBindings load(const QSettings& settings);
    void save(QSettings& settings) const;

    int keyModifiers() const;
    int keyScancode() const;
    quint32 controllerButtons() const;
    bool legacyGamepadDisconnect() const;

    bool setKeyboardBinding(int keyModifiers, int keyScancode);
    bool setControllerBinding(quint32 controllerButtons);
    void setLegacyGamepadDisconnect(bool enabled);
    void resetToDefaults();

    static bool isValidKeyboardBinding(int keyModifiers, int keyScancode);
    static bool isValidControllerBinding(quint32 controllerButtons);
    static bool conflictsWithStatsChord(quint32 controllerButtons);
    static QString controllerConflictReason(quint32 controllerButtons);
    static QString formatKeyboardBinding(int keyModifiers, int keyScancode);
    static QString formatControllerBinding(quint32 controllerButtons);
    static int sdlScancodeForQtKey(int qtKey);
    static int sdlScancodeForNativeKey(quint32 nativeScanCode,
                                       const QString& platformName);

    static quint32 defaultControllerButtons();
    static quint32 statsControllerButtons();

    static constexpr int DefaultKeyModifiers =
        int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    static constexpr int DefaultKeyScancode = SDL_SCANCODE_SPACE;

private:
    int m_KeyModifiers = DefaultKeyModifiers;
    int m_KeyScancode = DefaultKeyScancode;
    quint32 m_ControllerButtons = 0;
    bool m_LegacyGamepadDisconnect = false;
};

class DeckControllerChordCapture final
{
public:
    void begin();
    void cancel();
    bool isActive() const;
    std::optional<quint32> handleButton(Uint8 button, bool pressed);

private:
    bool m_Active = false;
    quint32 m_ButtonsDown = 0;
    quint32 m_PeakButtons = 0;
};
