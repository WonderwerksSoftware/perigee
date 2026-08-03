#include "overlaymanager.h"
#include "path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

bool Overlay::composeOverlaySurfacePatch(SDL_Surface* destination,
                                         SDL_Surface* source,
                                         const SDL_Rect& previousRect,
                                         const SDL_Rect& newRect,
                                         SDL_Rect* damagedRect)
{
    if (destination == nullptr || source == nullptr || damagedRect == nullptr ||
            destination->format == nullptr || source->format == nullptr ||
            destination->pixels == nullptr || source->pixels == nullptr ||
            destination->format->format != source->format->format ||
            destination->format->BytesPerPixel <= 0 ||
            destination->format->BytesPerPixel != source->format->BytesPerPixel ||
            destination->pitch <= 0 || source->pitch <= 0 ||
            newRect.w != source->w || newRect.h != source->h) {
        return false;
    }

    const auto rectWithinSurface = [](const SDL_Rect& rect,
                                      int width,
                                      int height,
                                      bool allowEmpty) {
        if (rect.w < 0 || rect.h < 0 || rect.x < 0 || rect.y < 0) {
            return false;
        }
        if (rect.w == 0 || rect.h == 0) {
            return allowEmpty;
        }
        return static_cast<int64_t>(rect.x) + rect.w <= width &&
               static_cast<int64_t>(rect.y) + rect.h <= height;
    };
    if (!rectWithinSurface(
            newRect, destination->w, destination->h, false) ||
            !rectWithinSurface(
                previousRect, destination->w, destination->h, true)) {
        return false;
    }

    SDL_Rect unionRect = newRect;
    if (previousRect.w > 0 && previousRect.h > 0) {
        const int left = std::min(previousRect.x, newRect.x);
        const int top = std::min(previousRect.y, newRect.y);
        const int64_t right = std::max(
            static_cast<int64_t>(previousRect.x) + previousRect.w,
            static_cast<int64_t>(newRect.x) + newRect.w);
        const int64_t bottom = std::max(
            static_cast<int64_t>(previousRect.y) + previousRect.h,
            static_cast<int64_t>(newRect.y) + newRect.h);
        if (right - left > std::numeric_limits<int>::max() ||
                bottom - top > std::numeric_limits<int>::max()) {
            return false;
        }
        unionRect = {
            left,
            top,
            static_cast<int>(right - left),
            static_cast<int>(bottom - top),
        };
    }

    const auto checkedMultiply = [](size_t left,
                                    size_t right,
                                    size_t* product) {
        if (left != 0 && right > std::numeric_limits<size_t>::max() / left) {
            return false;
        }
        *product = left * right;
        return true;
    };

    const size_t bytesPerPixel = destination->format->BytesPerPixel;
    size_t sourceRowBytes;
    size_t destinationVisibleRowBytes;
    size_t patchRowBytes;
    size_t patchBytes;
    size_t sourceStorageBytes;
    size_t destinationStorageBytes;
    size_t patchXBytes;
    size_t unionXBytes;
    const size_t patchX = static_cast<size_t>(newRect.x - unionRect.x);
    const size_t patchY = static_cast<size_t>(newRect.y - unionRect.y);
    if (!checkedMultiply(source->w, bytesPerPixel, &sourceRowBytes) ||
            !checkedMultiply(destination->w, bytesPerPixel,
                             &destinationVisibleRowBytes) ||
            !checkedMultiply(unionRect.w, bytesPerPixel, &patchRowBytes) ||
            !checkedMultiply(patchRowBytes, unionRect.h, &patchBytes) ||
            !checkedMultiply(source->pitch, source->h, &sourceStorageBytes) ||
            !checkedMultiply(destination->pitch, destination->h,
                             &destinationStorageBytes) ||
            !checkedMultiply(patchX, bytesPerPixel, &patchXBytes) ||
            !checkedMultiply(unionRect.x, bytesPerPixel, &unionXBytes) ||
            sourceRowBytes > static_cast<size_t>(source->pitch) ||
            destinationVisibleRowBytes > static_cast<size_t>(destination->pitch) ||
            patchY + static_cast<size_t>(source->h) >
                static_cast<size_t>(unionRect.h) ||
            patchXBytes > patchRowBytes ||
            sourceRowBytes > patchRowBytes - patchXBytes ||
            unionXBytes > static_cast<size_t>(destination->pitch) ||
            patchRowBytes > static_cast<size_t>(destination->pitch) - unionXBytes) {
        return false;
    }

    auto* patch = static_cast<uint8_t*>(SDL_calloc(1, patchBytes));
    if (patch == nullptr) {
        return false;
    }

    for (size_t sourceY = 0; sourceY < static_cast<size_t>(source->h); sourceY++) {
        const size_t sourceOffset = sourceY * static_cast<size_t>(source->pitch);
        const size_t patchOffset =
            (patchY + sourceY) * patchRowBytes + patchXBytes;
        SDL_assert(sourceOffset + sourceRowBytes <= sourceStorageBytes);
        SDL_assert(patchOffset + sourceRowBytes <= patchBytes);
        memcpy(patch + patchOffset,
               static_cast<const uint8_t*>(source->pixels) + sourceOffset,
               sourceRowBytes);
    }

    for (size_t patchYIndex = 0;
         patchYIndex < static_cast<size_t>(unionRect.h);
         patchYIndex++) {
        const size_t destinationOffset =
            (static_cast<size_t>(unionRect.y) + patchYIndex) *
                static_cast<size_t>(destination->pitch) +
            unionXBytes;
        SDL_assert(destinationOffset + patchRowBytes <= destinationStorageBytes);
        memcpy(static_cast<uint8_t*>(destination->pixels) + destinationOffset,
               patch + patchYIndex * patchRowBytes,
               patchRowBytes);
    }

    SDL_free(patch);
    *damagedRect = unionRect;
    return true;
}

OverlayLayerCompositor::OverlayLayerCompositor(
        int canvasWidth,
        int canvasHeight,
        OverlaySurfaceDeleter surfaceDeleter) :
    m_CanvasWidth(canvasWidth),
    m_CanvasHeight(canvasHeight),
    m_SurfaceDeleter(std::move(surfaceDeleter))
{
}

OverlayLayerCompositor::~OverlayLayerCompositor()
{
    for (Layer& layer : m_Layers) {
        freeSurface(layer.surface);
    }
}

bool OverlayLayerCompositor::updateLayer(
        OverlayType type,
        SDL_Surface* ownedPremultipliedSurface,
        SDL_Rect rect,
        SDL_Surface* destination,
        SDL_Rect* damagedRect)
{
    const bool validType = type >= OverlayDebug && type < OverlayMax;
    const bool hasSurface = ownedPremultipliedSurface != nullptr;
    const bool validDestination = destination == nullptr ||
        (damagedRect != nullptr &&
         destination->w == m_CanvasWidth &&
         destination->h == m_CanvasHeight);
    const Layer newLayer {
        ownedPremultipliedSurface,
        hasSurface ? rect : SDL_Rect {},
    };
    if (!validType || !validDestination || !validateLayer(newLayer)) {
        freeSurface(ownedPremultipliedSurface);
        return false;
    }

    Layer& current = m_Layers[type];
    SDL_Rect damage {};
    if (current.surface != nullptr && hasSurface) {
        SDL_UnionRect(&current.rect, &rect, &damage);
    }
    else if (current.surface != nullptr) {
        damage = current.rect;
    }
    else if (hasSurface) {
        damage = rect;
    }

    Layer tentativeLayers[OverlayMax];
    std::copy(std::begin(m_Layers), std::end(m_Layers), tentativeLayers);
    tentativeLayers[type] = newLayer;
    if (destination != nullptr &&
            damage.w > 0 && damage.h > 0 &&
            !composeDamage(destination, tentativeLayers, damage)) {
        freeSurface(ownedPremultipliedSurface);
        return false;
    }

    SDL_Surface* oldSurface = current.surface;
    current = newLayer;
    if (oldSurface != ownedPremultipliedSurface) {
        freeSurface(oldSurface);
    }
    if (damagedRect != nullptr) {
        *damagedRect = damage;
    }
    return true;
}

bool OverlayLayerCompositor::composeAll(
        SDL_Surface* destination,
        SDL_Rect* damagedRect) const
{
    if (destination == nullptr || damagedRect == nullptr ||
            destination->w != m_CanvasWidth ||
            destination->h != m_CanvasHeight ||
            m_CanvasWidth <= 0 || m_CanvasHeight <= 0) {
        return false;
    }

    const SDL_Rect fullCanvas {0, 0, m_CanvasWidth, m_CanvasHeight};
    if (!composeDamage(destination, m_Layers, fullCanvas)) {
        return false;
    }
    *damagedRect = fullCanvas;
    return true;
}

bool OverlayLayerCompositor::validateLayer(const Layer& layer) const
{
    if (layer.surface == nullptr) {
        return layer.rect.w == 0 && layer.rect.h == 0;
    }

    const SDL_Surface* surface = layer.surface;
    if (surface->format == nullptr || surface->pixels == nullptr ||
            surface->format->format != SDL_PIXELFORMAT_ARGB8888 ||
            surface->format->BytesPerPixel != 4 ||
            surface->pitch <= 0 ||
            layer.rect.x < 0 || layer.rect.y < 0 ||
            layer.rect.w <= 0 || layer.rect.h <= 0 ||
            layer.rect.w != surface->w || layer.rect.h != surface->h ||
            static_cast<int64_t>(layer.rect.x) + layer.rect.w > m_CanvasWidth ||
            static_cast<int64_t>(layer.rect.y) + layer.rect.h > m_CanvasHeight) {
        return false;
    }

    size_t visibleRowBytes;
    const size_t bytesPerPixel = surface->format->BytesPerPixel;
    if (static_cast<size_t>(surface->w) >
            std::numeric_limits<size_t>::max() / bytesPerPixel) {
        return false;
    }
    visibleRowBytes = static_cast<size_t>(surface->w) * bytesPerPixel;
    return visibleRowBytes <= static_cast<size_t>(surface->pitch) &&
           static_cast<size_t>(surface->h) <=
               std::numeric_limits<size_t>::max() /
                   static_cast<size_t>(surface->pitch);
}

bool OverlayLayerCompositor::composeDamage(
        SDL_Surface* destination,
        const Layer layers[OverlayMax],
        SDL_Rect damageRect) const
{
    if (destination == nullptr || destination->format == nullptr ||
            destination->pixels == nullptr ||
            destination->format->format != SDL_PIXELFORMAT_ARGB8888 ||
            destination->format->BytesPerPixel != 4 ||
            destination->pitch <= 0 ||
            destination->w != m_CanvasWidth ||
            destination->h != m_CanvasHeight ||
            damageRect.x < 0 || damageRect.y < 0 ||
            damageRect.w <= 0 || damageRect.h <= 0 ||
            static_cast<int64_t>(damageRect.x) + damageRect.w > m_CanvasWidth ||
            static_cast<int64_t>(damageRect.y) + damageRect.h > m_CanvasHeight) {
        return false;
    }
    for (int type = OverlayDebug; type < OverlayMax; type++) {
        if (!validateLayer(layers[type])) {
            return false;
        }
    }

    const size_t bytesPerPixel = destination->format->BytesPerPixel;
    const auto checkedMultiply = [](size_t left,
                                    size_t right,
                                    size_t* product) {
        if (left != 0 && right > std::numeric_limits<size_t>::max() / left) {
            return false;
        }
        *product = left * right;
        return true;
    };
    size_t destinationVisibleRowBytes;
    size_t destinationStorageBytes;
    size_t patchRowBytes;
    size_t patchBytes;
    size_t damageXBytes;
    if (!checkedMultiply(destination->w, bytesPerPixel,
                         &destinationVisibleRowBytes) ||
            !checkedMultiply(destination->pitch, destination->h,
                             &destinationStorageBytes) ||
            !checkedMultiply(damageRect.w, bytesPerPixel, &patchRowBytes) ||
            !checkedMultiply(patchRowBytes, damageRect.h, &patchBytes) ||
            !checkedMultiply(damageRect.x, bytesPerPixel, &damageXBytes) ||
            destinationVisibleRowBytes > static_cast<size_t>(destination->pitch) ||
            damageXBytes > static_cast<size_t>(destination->pitch) ||
            patchRowBytes > static_cast<size_t>(destination->pitch) - damageXBytes) {
        return false;
    }

    auto* patch = static_cast<uint8_t*>(SDL_calloc(1, patchBytes));
    if (patch == nullptr) {
        return false;
    }

    static_assert(OverlayDebug < OverlayStatusUpdate &&
                  OverlayStatusUpdate < OverlayDeck,
                  "Overlay enum order defines DRM composition z-order");
    for (int type = OverlayDebug; type < OverlayMax; type++) {
        const Layer& layer = layers[type];
        if (layer.surface == nullptr) {
            continue;
        }

        SDL_Rect intersection;
        if (!SDL_IntersectRect(&layer.rect, &damageRect, &intersection)) {
            continue;
        }

        const size_t sourceX = intersection.x - layer.rect.x;
        const size_t sourceY = intersection.y - layer.rect.y;
        const size_t patchX = intersection.x - damageRect.x;
        const size_t patchY = intersection.y - damageRect.y;
        const size_t sourcePitch = layer.surface->pitch;
        for (int rowIndex = 0; rowIndex < intersection.h; rowIndex++) {
            const auto* sourceRow =
                static_cast<const uint8_t*>(layer.surface->pixels) +
                (sourceY + rowIndex) * sourcePitch +
                sourceX * bytesPerPixel;
            auto* patchRow = patch +
                (patchY + rowIndex) * patchRowBytes +
                patchX * bytesPerPixel;
            for (int columnIndex = 0;
                 columnIndex < intersection.w;
                 columnIndex++) {
                uint32_t sourcePixel;
                uint32_t destinationPixel;
                memcpy(&sourcePixel,
                       sourceRow + columnIndex * bytesPerPixel,
                       sizeof(sourcePixel));
                if ((sourcePixel & layer.surface->format->Amask) == 0) {
                    continue;
                }
                if ((sourcePixel & layer.surface->format->Amask) ==
                        layer.surface->format->Amask) {
                    memcpy(patchRow + columnIndex * bytesPerPixel,
                           &sourcePixel,
                           sizeof(sourcePixel));
                    continue;
                }

                memcpy(&destinationPixel,
                       patchRow + columnIndex * bytesPerPixel,
                       sizeof(destinationPixel));
                Uint8 sourceRed, sourceGreen, sourceBlue, sourceAlpha;
                Uint8 destinationRed, destinationGreen, destinationBlue,
                    destinationAlpha;
                SDL_GetRGBA(sourcePixel,
                            layer.surface->format,
                            &sourceRed,
                            &sourceGreen,
                            &sourceBlue,
                            &sourceAlpha);
                SDL_GetRGBA(destinationPixel,
                            destination->format,
                            &destinationRed,
                            &destinationGreen,
                            &destinationBlue,
                            &destinationAlpha);
                const unsigned inverseAlpha = 255 - sourceAlpha;
                const auto overChannel = [inverseAlpha](Uint8 source,
                                                        Uint8 destinationValue) {
                    return static_cast<Uint8>(std::min(
                        255u,
                        static_cast<unsigned>(source) +
                            (static_cast<unsigned>(destinationValue) *
                                 inverseAlpha +
                             127) /
                                255));
                };
                const uint32_t outputPixel = SDL_MapRGBA(
                    destination->format,
                    overChannel(sourceRed, destinationRed),
                    overChannel(sourceGreen, destinationGreen),
                    overChannel(sourceBlue, destinationBlue),
                    overChannel(sourceAlpha, destinationAlpha));
                memcpy(patchRow + columnIndex * bytesPerPixel,
                       &outputPixel,
                       sizeof(outputPixel));
            }
        }
    }

    for (int rowIndex = 0; rowIndex < damageRect.h; rowIndex++) {
        const size_t destinationOffset =
            (static_cast<size_t>(damageRect.y) + rowIndex) *
                static_cast<size_t>(destination->pitch) +
            damageXBytes;
        SDL_assert(destinationOffset + patchRowBytes <=
                   destinationStorageBytes);
        memcpy(static_cast<uint8_t*>(destination->pixels) + destinationOffset,
               patch + static_cast<size_t>(rowIndex) * patchRowBytes,
               patchRowBytes);
    }

    SDL_free(patch);
    return true;
}

void OverlayLayerCompositor::freeSurface(SDL_Surface* surface) const
{
    if (surface != nullptr) {
        m_SurfaceDeleter(surface);
    }
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

bool OverlayRendererReadiness::deferIfNotReady(OverlayType type)
{
    SDL_assert(type >= OverlayDebug && type < OverlayMax);
    if (m_Ready) {
        return false;
    }

    m_Deferred[type] = true;
    return true;
}

std::vector<OverlayType> OverlayRendererReadiness::activate()
{
    std::vector<OverlayType> deferred;
    deferred.reserve(OverlayMax);
    for (int type = OverlayDebug; type < OverlayMax; type++) {
        if (m_Deferred[type]) {
            deferred.push_back(static_cast<OverlayType>(type));
            m_Deferred[type] = false;
        }
    }
    m_Ready = true;
    return deferred;
}

void OverlayRendererReadiness::deactivate()
{
    m_Ready = false;
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
    SDL_Surface* retiredSurface = nullptr;
    {
        std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);
        SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
        if (m_Overlays[type].enabled.load()) {
            retiredSurface = notifyOverlayUpdatedLocked(type);
        }
    }
    freeSurface(retiredSurface);
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
    SDL_Surface* retiredSurface = nullptr;
    {
        std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);
        retiredSurface = updateOverlaySurfaceLocked(type, ownedSurface, presentation);
    }
    freeSurface(retiredSurface);
}

SDL_Surface* OverlayManager::updateOverlaySurfaceLocked(OverlayType type,
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

    return oldSurface;
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    SDL_Surface* retiredSurface = nullptr;
    {
        std::lock_guard<std::mutex> notificationLock(m_NotificationMutexes[type]);

        // Only update the overlay state if it's enabled. If it's not enabled,
        // the renderer has already been notified by setOverlayState().
        if (m_Overlays[type].enabled.load()) {
            retiredSurface = notifyOverlayUpdatedLocked(type);
        }
    }
    freeSurface(retiredSurface);
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    SDL_Surface* retiredSurface = nullptr;
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
                    retiredSurface = updateOverlaySurfaceLocked(
                        type,
                        nullptr,
                        {OverlayAnchor::TopCenter, 0, 1.0f, 1.0f});
                }
            }
            else {
                retiredSurface = notifyOverlayUpdatedLocked(type);
            }
        }
    }
    freeSurface(retiredSurface);
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    static_assert(OverlayMax == 3,
                  "Update the all-overlay renderer lock when adding an overlay type");
    std::scoped_lock notificationLocks(
        m_NotificationMutexes[OverlayDebug],
        m_NotificationMutexes[OverlayStatusUpdate],
        m_NotificationMutexes[OverlayDeck]);
    m_Renderer = renderer;
}

void OverlayManager::freeSurface(SDL_Surface* surface)
{
    if (surface != nullptr) {
        m_SurfaceDeleter(surface);
    }
}

SDL_Surface* OverlayManager::notifyOverlayUpdatedLocked(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return nullptr;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return nullptr;
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
            return nullptr;
        }
    }

    OverlayPresentation presentation;
    if (type == OverlayStatusUpdate) {
        presentation.anchor = OverlayAnchor::BottomLeft;
    }

    return updateOverlaySurfaceLocked(
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
