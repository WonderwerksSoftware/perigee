#pragma once

#include <QString>

#include <functional>
#include <atomic>
#include <mutex>

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

using OverlaySurfaceDeleter = std::function<void(SDL_Surface*)>;

SDL_FRect calculateOverlayRect(OverlayPresentation presentation,
                               int surfaceWidth, int surfaceHeight,
                               int viewportWidth, int viewportHeight,
                               bool originAtBottomLeft = false);

bool composeOverlaySurfacePatch(SDL_Surface* destination,
                                SDL_Surface* source,
                                const SDL_Rect& previousRect,
                                const SDL_Rect& newRect,
                                SDL_Rect* damagedRect);

class OverlayLayoutState
{
public:
    void setSurface(int surfaceWidth,
                    int surfaceHeight,
                    OverlayPresentation presentation);
    void clear();
    bool hasSurface() const;
    bool updateLayout(int viewportWidth,
                      int viewportHeight,
                      bool originAtBottomLeft,
                      SDL_FRect* rect);

private:
    bool m_HasSurface = false;
    bool m_HasLayout = false;
    int m_SurfaceWidth = 0;
    int m_SurfaceHeight = 0;
    int m_ViewportWidth = 0;
    int m_ViewportHeight = 0;
    bool m_OriginAtBottomLeft = false;
    OverlayPresentation m_Presentation;
    SDL_FRect m_Rect = {};
};

class SingleOverlayArbiter
{
public:
    explicit SingleOverlayArbiter(
        OverlaySurfaceDeleter surfaceDeleter = SDL_FreeSurface);
    ~SingleOverlayArbiter();

    bool update(OverlayType type,
                SDL_Surface* ownedSurface,
                OverlayPresentation presentation,
                bool enabled);
    bool getSelection(OverlayType* type,
                      SDL_Surface** surface,
                      OverlayPresentation* presentation) const;

private:
    struct RetainedSurface {
        SDL_Surface* surface = nullptr;
        OverlayPresentation presentation;
    };

    OverlayType selectType() const;
    void freeSurface(SDL_Surface* surface);

    RetainedSurface m_Retained[OverlayMax];
    OverlayType m_SelectedType = OverlayMax;
    OverlaySurfaceDeleter m_SurfaceDeleter;
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    // OverlayManager serializes this callback per OverlayType. Implementations
    // may consume the pending update, but must not synchronously submit another
    // update of the same type from this callback.
    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    explicit OverlayManager(
        OverlaySurfaceDeleter surfaceDeleter = SDL_FreeSurface);
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

    // Waits for every in-flight overlay callback before replacing the renderer.
    // This must not be called from within IOverlayRenderer::notifyOverlayUpdated().
    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    SDL_Surface* notifyOverlayUpdatedLocked(OverlayType type);
    SDL_Surface* updateOverlaySurfaceLocked(OverlayType type,
                                            SDL_Surface* ownedSurface,
                                            OverlayPresentation presentation);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);
    void freeSurface(SDL_Surface* surface);

    struct {
        std::atomic_bool enabled;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        bool hasPendingSurface;
        SDL_Surface* pendingSurface;
        OverlayPresentation pendingPresentation;
    } m_Overlays[OverlayMax];
    std::mutex m_PendingMutexes[OverlayMax];
    std::mutex m_NotificationMutexes[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    OverlaySurfaceDeleter m_SurfaceDeleter;
};

}
