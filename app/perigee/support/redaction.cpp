#include "redaction.h"

#include <QSet>
#include <QStringList>
#include <QUrl>

namespace
{
bool isBodyOrHeaderLike(const QString& input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.startsWith(QLatin1Char('{')) ||
            trimmed.startsWith(QLatin1Char('['))) {
        return true;
    }

    const QString lower = trimmed.toLower();
    return lower.contains(QStringLiteral("authorization:")) ||
           lower.contains(QStringLiteral("certificate:")) ||
           lower.contains(QStringLiteral("clipboard:")) ||
           lower.contains(QStringLiteral("command:")) ||
           lower.contains(QStringLiteral("cookie:")) ||
           lower.contains(QStringLiteral("key:")) ||
           lower.contains(QStringLiteral("session:")) ||
           lower.contains(QStringLiteral("set-cookie:")) ||
           lower.contains(QStringLiteral("token:")) ||
           lower.contains(QStringLiteral("private-key")) ||
           lower.contains(QStringLiteral("private key")) ||
           lower.contains(QStringLiteral("begin certificate"));
}

bool hasControlCharacters(const QString& input)
{
    for (const QChar character : input) {
        if (character.category() == QChar::Other_Control ||
                character == QChar::LineSeparator ||
                character == QChar::ParagraphSeparator) {
            return true;
        }
    }
    return false;
}
}

QString PerigeeRedaction::pathForLog(const QString& input)
{
    if (input.isEmpty() || input.size() > 4096 ||
            hasControlCharacters(input) || isBodyOrHeaderLike(input)) {
        return QStringLiteral("<redacted>");
    }

    const int fragmentIndex = input.indexOf(QLatin1Char('#'));
    const QString withoutFragment = fragmentIndex < 0
        ? input : input.left(fragmentIndex);
    const QUrl parsed(withoutFragment, QUrl::StrictMode);
    if (!parsed.isValid() || !parsed.scheme().isEmpty() ||
            !parsed.authority().isEmpty() || !parsed.userInfo().isEmpty()) {
        return QStringLiteral("<redacted>");
    }

    const int queryIndex = withoutFragment.indexOf(QLatin1Char('?'));
    const QString rawPath = queryIndex < 0
        ? withoutFragment : withoutFragment.left(queryIndex);
    QStringList segments = rawPath.split(QLatin1Char('/'));
    static const QSet<QString> sensitiveSegments = {
        QStringLiteral("authorization"),
        QStringLiteral("certificate"),
        QStringLiteral("certificates"),
        QStringLiteral("clipboard"),
        QStringLiteral("clipboards"),
        QStringLiteral("command"),
        QStringLiteral("commands"),
        QStringLiteral("key"),
        QStringLiteral("keys"),
        QStringLiteral("private-key"),
        QStringLiteral("session"),
        QStringLiteral("sessions"),
        QStringLiteral("token"),
        QStringLiteral("tokens"),
    };
    bool redactNext = false;
    for (QString& segment : segments) {
        if (redactNext && !segment.isEmpty()) {
            segment = QStringLiteral("<redacted>");
            redactNext = false;
            continue;
        }
        redactNext = sensitiveSegments.contains(segment.toLower());
    }

    QString result = segments.join(QLatin1Char('/'));
    if (queryIndex >= 0) {
        const QString query = withoutFragment.mid(queryIndex + 1);
        QStringList redactedPairs;
        const QStringList pairs = query.split(QLatin1Char('&'), Qt::KeepEmptyParts);
        redactedPairs.reserve(pairs.size());
        for (const QString& pair : pairs) {
            const int equals = pair.indexOf(QLatin1Char('='));
            const QString name = equals < 0 ? pair : pair.left(equals);
            redactedPairs.append(name + QStringLiteral("=<redacted>"));
        }
        result += QLatin1Char('?') + redactedPairs.join(QLatin1Char('&'));
    }
    return result.left(1024);
}
