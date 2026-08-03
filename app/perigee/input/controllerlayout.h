#pragma once

#include "SDL_compat.h"

#include <QObject>
#include <QString>
#include <QUrl>

class ControllerLayout final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Family family READ family NOTIFY layoutChanged)
    Q_PROPERTY(bool swapFaceButtons READ swapFaceButtons NOTIFY layoutChanged)
    Q_PROPERTY(QString confirmLabel READ confirmLabel NOTIFY layoutChanged)
    Q_PROPERTY(QString backLabel READ backLabel NOTIFY layoutChanged)
    Q_PROPERTY(QString searchLabel READ searchLabel NOTIFY layoutChanged)
    Q_PROPERTY(QString previousCategoryLabel READ previousCategoryLabel
               NOTIFY layoutChanged)
    Q_PROPERTY(QString nextCategoryLabel READ nextCategoryLabel
               NOTIFY layoutChanged)
    Q_PROPERTY(QUrl confirmGlyph READ confirmGlyph NOTIFY layoutChanged)
    Q_PROPERTY(QUrl backGlyph READ backGlyph NOTIFY layoutChanged)
    Q_PROPERTY(QUrl searchGlyph READ searchGlyph NOTIFY layoutChanged)
    Q_PROPERTY(QUrl previousCategoryGlyph READ previousCategoryGlyph
               NOTIFY layoutChanged)
    Q_PROPERTY(QUrl nextCategoryGlyph READ nextCategoryGlyph
               NOTIFY layoutChanged)

public:
    enum class Family {
        Unknown,
        Xbox,
        PlayStation,
        Nintendo,
        SteamDeck,
    };
    Q_ENUM(Family)

    explicit ControllerLayout(QObject* parent = nullptr);

    Family family() const;
    bool swapFaceButtons() const;
    QString confirmLabel() const;
    QString backLabel() const;
    QString searchLabel() const;
    QString previousCategoryLabel() const;
    QString nextCategoryLabel() const;
    QUrl confirmGlyph() const;
    QUrl backGlyph() const;
    QUrl searchGlyph() const;
    QUrl previousCategoryGlyph() const;
    QUrl nextCategoryGlyph() const;

    void configure(Family family, bool swapFaceButtons);

    static Family familyForSdl(SDL_GameControllerType type,
                               const QString& deviceName);
    static Family familyForController(SDL_JoystickID controllerId);

signals:
    void layoutChanged();

private:
    enum class FaceButton {
        A,
        B,
        X,
        Y,
    };

    QString faceLabel(FaceButton button) const;
    QUrl faceGlyph(FaceButton button) const;
    QUrl shoulderGlyph(bool right) const;
    Family effectiveFamily() const;

    Family m_Family = Family::Unknown;
    bool m_SwapFaceButtons = false;
};
