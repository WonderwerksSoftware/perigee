#include "controllerlayout.h"

namespace {

QUrl glyphUrl(const QString& fileName)
{
    return QUrl(QStringLiteral("qrc:/gui/perigee/glyphs/") + fileName);
}

}

ControllerLayout::ControllerLayout(QObject* parent)
    : QObject(parent)
{
}

ControllerLayout::Family ControllerLayout::family() const
{
    return m_Family;
}

bool ControllerLayout::swapFaceButtons() const
{
    return m_SwapFaceButtons;
}

QString ControllerLayout::confirmLabel() const
{
    return faceLabel(m_SwapFaceButtons ? FaceButton::B : FaceButton::A);
}

QString ControllerLayout::backLabel() const
{
    return faceLabel(m_SwapFaceButtons ? FaceButton::A : FaceButton::B);
}

QString ControllerLayout::searchLabel() const
{
    return faceLabel(m_SwapFaceButtons ? FaceButton::X : FaceButton::Y);
}

QString ControllerLayout::previousCategoryLabel() const
{
    switch (effectiveFamily()) {
    case Family::Xbox:
        return QStringLiteral("LB");
    case Family::PlayStation:
    case Family::SteamDeck:
        return QStringLiteral("L1");
    case Family::Nintendo:
        return QStringLiteral("L");
    case Family::Unknown:
        break;
    }
    return {};
}

QString ControllerLayout::nextCategoryLabel() const
{
    switch (effectiveFamily()) {
    case Family::Xbox:
        return QStringLiteral("RB");
    case Family::PlayStation:
    case Family::SteamDeck:
        return QStringLiteral("R1");
    case Family::Nintendo:
        return QStringLiteral("R");
    case Family::Unknown:
        break;
    }
    return {};
}

QUrl ControllerLayout::confirmGlyph() const
{
    return faceGlyph(m_SwapFaceButtons ? FaceButton::B : FaceButton::A);
}

QUrl ControllerLayout::backGlyph() const
{
    return faceGlyph(m_SwapFaceButtons ? FaceButton::A : FaceButton::B);
}

QUrl ControllerLayout::searchGlyph() const
{
    return faceGlyph(m_SwapFaceButtons ? FaceButton::X : FaceButton::Y);
}

QUrl ControllerLayout::previousCategoryGlyph() const
{
    return shoulderGlyph(false);
}

QUrl ControllerLayout::nextCategoryGlyph() const
{
    return shoulderGlyph(true);
}

void ControllerLayout::configure(Family family, bool swapFaceButtons)
{
    if (m_Family == family && m_SwapFaceButtons == swapFaceButtons) {
        return;
    }
    m_Family = family;
    m_SwapFaceButtons = swapFaceButtons;
    emit layoutChanged();
}

ControllerLayout::Family ControllerLayout::familyForSdl(
        SDL_GameControllerType type, const QString& deviceName)
{
    if (deviceName.contains(QStringLiteral("steam deck"),
                            Qt::CaseInsensitive)) {
        return Family::SteamDeck;
    }
    switch (type) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
        return Family::Xbox;
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
        return Family::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        return Family::Nintendo;
    default:
        return Family::Unknown;
    }
}

ControllerLayout::Family ControllerLayout::familyForController(
        SDL_JoystickID controllerId)
{
    SDL_GameController* controller =
        SDL_GameControllerFromInstanceID(controllerId);
    if (controller == nullptr) {
        return Family::Unknown;
    }
    const char* name = SDL_GameControllerName(controller);
    return familyForSdl(SDL_GameControllerGetType(controller),
                        name == nullptr ? QString()
                                        : QString::fromUtf8(name));
}

QString ControllerLayout::faceLabel(FaceButton button) const
{
    switch (effectiveFamily()) {
    case Family::PlayStation:
        switch (button) {
        case FaceButton::A: return QStringLiteral("Cross");
        case FaceButton::B: return QStringLiteral("Circle");
        case FaceButton::X: return QStringLiteral("Square");
        case FaceButton::Y: return QStringLiteral("Triangle");
        }
        break;
    case Family::Nintendo:
        switch (button) {
        case FaceButton::A: return QStringLiteral("B");
        case FaceButton::B: return QStringLiteral("A");
        case FaceButton::X: return QStringLiteral("Y");
        case FaceButton::Y: return QStringLiteral("X");
        }
        break;
    case Family::Xbox:
    case Family::SteamDeck:
    case Family::Unknown:
        switch (button) {
        case FaceButton::A: return QStringLiteral("A");
        case FaceButton::B: return QStringLiteral("B");
        case FaceButton::X: return QStringLiteral("X");
        case FaceButton::Y: return QStringLiteral("Y");
        }
        break;
    }
    return {};
}

QUrl ControllerLayout::faceGlyph(FaceButton button) const
{
    QString suffix;
    switch (button) {
    case FaceButton::A: suffix = QStringLiteral("a.svg"); break;
    case FaceButton::B: suffix = QStringLiteral("b.svg"); break;
    case FaceButton::X: suffix = QStringLiteral("x.svg"); break;
    case FaceButton::Y: suffix = QStringLiteral("y.svg"); break;
    }

    switch (effectiveFamily()) {
    case Family::PlayStation:
        switch (button) {
        case FaceButton::A: suffix = QStringLiteral("cross.svg"); break;
        case FaceButton::B: suffix = QStringLiteral("circle.svg"); break;
        case FaceButton::X: suffix = QStringLiteral("square.svg"); break;
        case FaceButton::Y: suffix = QStringLiteral("triangle.svg"); break;
        }
        return glyphUrl(QStringLiteral("playstation-") + suffix);
    case Family::Nintendo:
        switch (button) {
        case FaceButton::A: suffix = QStringLiteral("b.svg"); break;
        case FaceButton::B: suffix = QStringLiteral("a.svg"); break;
        case FaceButton::X: suffix = QStringLiteral("y.svg"); break;
        case FaceButton::Y: suffix = QStringLiteral("x.svg"); break;
        }
        return glyphUrl(QStringLiteral("nintendo-") + suffix);
    case Family::SteamDeck:
        return glyphUrl(QStringLiteral("steamdeck-") + suffix);
    case Family::Xbox:
    case Family::Unknown:
        return glyphUrl(QStringLiteral("xbox-") + suffix);
    }
    return {};
}

QUrl ControllerLayout::shoulderGlyph(bool right) const
{
    switch (effectiveFamily()) {
    case Family::PlayStation:
        return glyphUrl(right ? QStringLiteral("playstation-r1.svg")
                              : QStringLiteral("playstation-l1.svg"));
    case Family::Nintendo:
        return glyphUrl(right ? QStringLiteral("nintendo-r.svg")
                              : QStringLiteral("nintendo-l.svg"));
    case Family::SteamDeck:
        return glyphUrl(right ? QStringLiteral("steamdeck-r1.svg")
                              : QStringLiteral("steamdeck-l1.svg"));
    case Family::Xbox:
    case Family::Unknown:
        return glyphUrl(right ? QStringLiteral("xbox-rb.svg")
                              : QStringLiteral("xbox-lb.svg"));
    }
    return {};
}

ControllerLayout::Family ControllerLayout::effectiveFamily() const
{
    return m_Family == Family::Unknown ? Family::Xbox : m_Family;
}
