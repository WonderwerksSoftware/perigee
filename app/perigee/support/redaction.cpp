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
    if (lower.contains(QStringLiteral("private-key")) ||
            lower.contains(QStringLiteral("private key")) ||
            lower.contains(QStringLiteral("begin certificate"))) {
        return true;
    }

    const int colon = trimmed.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        return false;
    }
    const QString headerName = trimmed.left(colon).simplified().toLower();
    static const QStringList sensitiveHeaderTerms = {
        QStringLiteral("authorization"),
        QStringLiteral("certificate"),
        QStringLiteral("clipboard"),
        QStringLiteral("command"),
        QStringLiteral("cookie"),
        QStringLiteral("key"),
        QStringLiteral("session"),
        QStringLiteral("token"),
    };
    for (const QString& term : sensitiveHeaderTerms) {
        if (headerName.contains(term)) {
            return true;
        }
    }
    return false;
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

bool isSafeQueryName(const QString& name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (const QChar character : name) {
        const ushort code = character.unicode();
        const bool alphaNumeric =
            (code >= 'a' && code <= 'z') ||
            (code >= 'A' && code <= 'Z') ||
            (code >= '0' && code <= '9');
        if (!alphaNumeric && character != QLatin1Char('-') &&
                character != QLatin1Char('_') &&
                character != QLatin1Char('.') &&
                character != QLatin1Char('~')) {
            return false;
        }
    }
    return true;
}

bool isSensitivePathLabel(const QString& label)
{
    static const QSet<QString> sensitiveTerms = {
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
    const QString lower = label.toLower();
    if (sensitiveTerms.contains(lower)) {
        return true;
    }

    QString component;
    for (const QChar character : lower) {
        if (character == QLatin1Char('-') ||
                character == QLatin1Char('_') ||
                character == QLatin1Char('.')) {
            if (sensitiveTerms.contains(component)) {
                return true;
            }
            component.clear();
        }
        else {
            component.append(character);
        }
    }
    return sensitiveTerms.contains(component);
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
    if (!rawPath.startsWith(QLatin1Char('/')) ||
            rawPath.contains(QLatin1Char('%')) ||
            rawPath.contains(QLatin1Char('\\')) ||
            rawPath.contains(QStringLiteral("//")) ||
            parsed.path(QUrl::FullyEncoded) != rawPath) {
        return QStringLiteral("<redacted>");
    }
    const QStringList rawSegments = rawPath.split(QLatin1Char('/'));
    if (rawSegments.contains(QStringLiteral(".")) ||
            rawSegments.contains(QStringLiteral(".."))) {
        return QStringLiteral("<redacted>");
    }
    QStringList segments = rawPath.split(QLatin1Char('/'));
    bool redactNext = false;
    for (QString& segment : segments) {
        const bool sensitiveLabel = isSensitivePathLabel(segment);
        if (redactNext && !segment.isEmpty()) {
            segment = QStringLiteral("<redacted>");
            redactNext = sensitiveLabel;
            continue;
        }
        redactNext = sensitiveLabel;
    }

    QString result = segments.join(QLatin1Char('/'));
    if (queryIndex >= 0) {
        const QString query = withoutFragment.mid(queryIndex + 1);
        QStringList redactedPairs;
        const QStringList pairs = query.split(QLatin1Char('&'), Qt::KeepEmptyParts);
        redactedPairs.reserve(pairs.size());
        for (const QString& pair : pairs) {
            const int equals = pair.indexOf(QLatin1Char('='));
            const QString rawName = equals < 0 ? pair : pair.left(equals);
            const QString name = isSafeQueryName(rawName)
                ? rawName : QStringLiteral("<redacted>");
            redactedPairs.append(name + QStringLiteral("=<redacted>"));
        }
        result += QLatin1Char('?') + redactedPairs.join(QLatin1Char('&'));
    }
    return result.left(1024);
}
