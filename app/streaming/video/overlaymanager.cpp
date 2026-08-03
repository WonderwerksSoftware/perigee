#include "overlaymanager.h"
#include "path.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Overlay;

SDL_FRect Overlay::calculateOverlayRect(OverlayPresentation presentation,
                                        int surfaceWidth, int surfaceHeight,
                                        int viewportWidth, int viewportHeight,
                                        bool originAtBottomLeft)
{
    const float viewportW = std::max(0, viewportWidth);
    const float viewportH = std::max(0, viewportHeight);
    const float margin = std::max(0, presentation.marginPx);
    const float insetX = std::min(margin, viewportW * 0.5f);
    const float insetY = std::min(margin, viewportH * 0.5f);
    const float effectiveWidth = std::max(0.0f, viewportW - insetX * 2.0f);
    const float effectiveHeight = std::max(0.0f, viewportH - insetY * 2.0f);

    const auto sanitizeRatio = [](float ratio) {
        return std::isfinite(ratio) ? std::clamp(ratio, 0.0f, 1.0f) : 0.0f;
    };
    const float maxWidth = effectiveWidth * sanitizeRatio(presentation.maxWidthRatio);
    const float maxHeight = effectiveHeight * sanitizeRatio(presentation.maxHeightRatio);

    float width = 0.0f;
    float height = 0.0f;
    if (surfaceWidth > 0 && surfaceHeight > 0) {
        const float scale = std::min({1.0f,
                                      maxWidth / static_cast<float>(surfaceWidth),
                                      maxHeight / static_cast<float>(surfaceHeight)});
        width = static_cast<float>(surfaceWidth) * scale;
        height = static_cast<float>(surfaceHeight) * scale;
    }

    float x = insetX;
    float y = insetY;
    switch (presentation.anchor) {
    case OverlayAnchor::TopLeft:
        break;
    case OverlayAnchor::TopCenter:
        x += (effectiveWidth - width) * 0.5f;
        break;
    case OverlayAnchor::BottomLeft:
        y += effectiveHeight - height;
        break;
    }

    if (originAtBottomLeft) {
        y = viewportH - y - height;
    }

    return {x, y, width, height};
}

void OverlayLayoutState::setSurface(int surfaceWidth,
                                    int surfaceHeight,
                                    OverlayPresentation presentation)
{
    m_HasSurface = surfaceWidth > 0 && surfaceHeight > 0;
    m_SurfaceWidth = surfaceWidth;
    m_SurfaceHeight = surfaceHeight;
    m_Presentation = presentation;
    m_HasLayout = false;
}

void OverlayLayoutState::clear()
{
    m_HasSurface = false;
    m_HasLayout = false;
    m_SurfaceWidth = 0;
    m_SurfaceHeight = 0;
    m_Rect = {};
}

bool OverlayLayoutState::hasSurface() const
{
    return m_HasSurface;
}

bool OverlayLayoutState::updateLayout(int viewportWidth,
                                      int viewportHeight,
                                      bool originAtBottomLeft,
                                      SDL_FRect* rect)
{
    SDL_assert(rect != nullptr);

    if (!m_HasSurface) {
        *rect = {};
        return false;
    }

    const bool changed = !m_HasLayout ||
                         m_ViewportWidth != viewportWidth ||
                         m_ViewportHeight != viewportHeight ||
                         m_OriginAtBottomLeft != originAtBottomLeft;
    if (changed) {
        m_Rect = calculateOverlayRect(
            m_Presentation,
            m_SurfaceWidth,
            m_SurfaceHeight,
            viewportWidth,
            viewportHeight,
            originAtBottomLeft);
        m_ViewportWidth = viewportWidth;
        m_ViewportHeight = viewportHeight;
        m_OriginAtBottomLeft = originAtBottomLeft;
        m_HasLayout = true;
    }

    *rect = m_Rect;
    return changed;
}

SingleOverlayArbiter::SingleOverlayArbiter(OverlaySurfaceDeleter surfaceDeleter) :
    m_SurfaceDeleter(std::move(surfaceDeleter))
{
}

SingleOverlayArbiter::~SingleOverlayArbiter()
{
    for (RetainedSurface& retained : m_Retained) {
        freeSurface(retained.surface);
    }
}

bool SingleOverlayArbiter::update(OverlayType type,
                                  SDL_Surface* ownedSurface,
                                  OverlayPresentation presentation,
                                  bool enabled)
{
    if (type != OverlayStatusUpdate && type != OverlayDeck) {
        freeSurface(ownedSurface);
        return false;
    }

    const OverlayType oldSelectedType = m_SelectedType;
    const bool replacedSelectedSurface = oldSelectedType == type &&
                                         enabled &&
                                         ownedSurface != nullptr;

    RetainedSurface& retained = m_Retained[type];
    freeSurface(retained.surface);
    retained.surface = nullptr;

    if (enabled && ownedSurface != nullptr) {
        retained.surface = ownedSurface;
        retained.presentation = presentation;
    }
    else {
        freeSurface(ownedSurface);
    }

    m_SelectedType = selectType();
    return m_SelectedType != oldSelectedType || replacedSelectedSurface;
}

bool SingleOverlayArbiter::getSelection(OverlayType* type,
                                        SDL_Surface** surface,
                                        OverlayPresentation* presentation) const
{
    SDL_assert(type != nullptr);
    SDL_assert(surface != nullptr);
    SDL_assert(presentation != nullptr);

    *type = m_SelectedType;
    if (m_SelectedType == OverlayMax) {
        *surface = nullptr;
        *presentation = {};
        return false;
    }

    *surface = m_Retained[m_SelectedType].surface;
    *presentation = m_Retained[m_SelectedType].presentation;
    return true;
}

OverlayType SingleOverlayArbiter::selectType() const
{
    if (m_Retained[OverlayStatusUpdate].surface != nullptr) {
        return OverlayStatusUpdate;
    }
    if (m_Retained[OverlayDeck].surface != nullptr) {
        return OverlayDeck;
    }
    return OverlayMax;
}

void SingleOverlayArbiter::freeSurface(SDL_Surface* surface)
{
    if (surface != nullptr) {
        m_SurfaceDeleter(surface);
    }
}

OverlayManager::OverlayManager(OverlaySurfaceDeleter surfaceDeleter) :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf")),
    m_SurfaceDeleter(std::move(surfaceDeleter))
{
    for (auto& overlay : m_Overlays) {
        overlay.enabled = false;
        overlay.fontSize = 0;
        overlay.color = {};
        memset(overlay.text, 0, sizeof(overlay.text));
        overlay.font = nullptr;
        overlay.hasPendingSurface = false;
        overlay.pendingSurface = nullptr;
        overlay.pendingPresentation = {};
    }

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        SDL_Surface* pendingSurface = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_PendingMutexes[i]);
            if (m_Overlays[i].hasPendingSurface) {
                pendingSurface = m_Overlays[i].pendingSurface;
                m_Overlays[i].hasPendingSurface = false;
                m_Overlays[i].pendingSurface = nullptr;
            }
        }
        freeSurface(pendingSurface);
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled.load();
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);
    SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    if (m_Overlays[type].enabled.load()) {
        notifyOverlayUpdatedLocked(type);
    }
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

bool OverlayManager::getUpdatedOverlaySurface(OverlayType type,
                                              SDL_Surface** ownedSurface,
                                              OverlayPresentation* presentation)
{
    SDL_assert(ownedSurface != nullptr);
    SDL_assert(presentation != nullptr);

    *ownedSurface = nullptr;
    std::lock_guard<std::mutex> lock(m_PendingMutexes[type]);
    if (!m_Overlays[type].hasPendingSurface) {
        return false;
    }

    *ownedSurface = m_Overlays[type].pendingSurface;
    *presentation = m_Overlays[type].pendingPresentation;
    m_Overlays[type].hasPendingSurface = false;
    m_Overlays[type].pendingSurface = nullptr;
    return true;
}

void OverlayManager::updateOverlaySurface(OverlayType type,
                                          SDL_Surface* ownedSurface,
                                          OverlayPresentation presentation)
{
    // The per-type notification lock serializes publication through renderer
    // consumption. Renderer callbacks must not synchronously submit another
    // update for the same type. No pending-state lock is held during callbacks.
    std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);
    updateOverlaySurfaceLocked(type, ownedSurface, presentation);
}

void OverlayManager::updateOverlaySurfaceLocked(OverlayType type,
                                                SDL_Surface* ownedSurface,
                                                OverlayPresentation presentation)
{
    SDL_Surface* oldSurface = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_PendingMutexes[type]);
        if (m_Overlays[type].hasPendingSurface) {
            oldSurface = m_Overlays[type].pendingSurface;
        }
        m_Overlays[type].hasPendingSurface = true;
        m_Overlays[type].pendingSurface = ownedSurface;
        m_Overlays[type].pendingPresentation = presentation;
    }

    if (m_Renderer != nullptr) {
        m_Renderer->notifyOverlayUpdated(type);
    }

    freeSurface(oldSurface);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);

    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled.load()) {
        notifyOverlayUpdatedLocked(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);
    const bool stateChanged = m_Overlays[type].enabled.load() != enabled;

    m_Overlays[type].enabled.store(enabled);

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        if (type == OverlayDeck) {
            // Deck is surface-backed rather than text-backed. Enabling waits for
            // the producer's first surface; disabling publishes an explicit clear.
            if (!enabled) {
                updateOverlaySurfaceLocked(
                    type,
                    nullptr,
                    {OverlayAnchor::TopCenter, 0, 1.0f, 1.0f});
            }
            return;
        }

        notifyOverlayUpdatedLocked(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::freeSurface(SDL_Surface* surface)
{
    if (surface != nullptr) {
        m_SurfaceDeleter(surface);
    }
}

void OverlayManager::notifyOverlayUpdatedLocked(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    OverlayPresentation presentation;
    if (type == OverlayStatusUpdate) {
        presentation.anchor = OverlayAnchor::BottomLeft;
    }

    updateOverlaySurfaceLocked(
        type,
        m_Overlays[type].enabled.load() ?
            // The _Wrapped variant is required for line breaks to work
            RenderTextOutlinedWrapped(m_Overlays[type].font,
                                      m_Overlays[type].text,
                                      m_Overlays[type].color,
                                      {0, 0, 0, 255},
                                      4,
                                      1024)
            : nullptr,
        presentation);
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}
