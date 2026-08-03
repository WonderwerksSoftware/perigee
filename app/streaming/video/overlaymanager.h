#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayDeck,
    OverlayMax
};

enum class OverlayAnchor {
    TopLeft,
    TopCenter,
    BottomLeft
};

struct OverlayPresentation {
    OverlayAnchor anchor = OverlayAnchor::TopLeft;
    int marginPx = 0;
    float maxWidthRatio = 1.0f;
    float maxHeightRatio = 1.0f;
};

SDL_FRect calculateOverlayRect(OverlayPresentation presentation,
                               int surfaceWidth, int surfaceHeight,
                               int viewportWidth, int viewportHeight,
                               bool originAtBottomLeft = false);

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    bool getUpdatedOverlaySurface(OverlayType type,
                                  SDL_Surface** ownedSurface,
                                  OverlayPresentation* presentation);
    void updateOverlaySurface(OverlayType type,
                              SDL_Surface* ownedSurface,
                              OverlayPresentation presentation);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct PendingSurface {
        SDL_Surface* surface;
        OverlayPresentation presentation;
    };

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        PendingSurface* pendingSurface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
