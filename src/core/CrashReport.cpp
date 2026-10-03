#include "core/CrashReport.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <csignal>

namespace rostrum::crash {

namespace {

const QString kMagic = QStringLiteral("rostrum-crash 1");

// Where each allowed field goes in the report. Nothing else in the file is ever read.
struct Field
{
    const char *key;
    const char *section;
};
constexpr Field kFields[] = {
    {"version", "app"},       {"build_id", "app"},     {"install", "app"},     {"signal", "crash"},
    {"uptime", "crash"},      {"os", "system"},        {"kernel", "system"},   {"arch", "system"},
    {"desktop", "system"},    {"session", "system"},   {"qt", "system"},       {"kf", "system"},
    {"pipewire", "system"},   {"wireplumber", "system"},
};

bool isHex(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]+$"));
    return re.match(s).hasMatch();
}

} // namespace

CrashFile parseCrashFile(const QByteArray &text)
{
    CrashFile file;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.constFirst().trimmed() != kMagic) {
        return file;
    }
    bool inFrames = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (inFrames) {
            if (line.startsWith(QLatin1String("===="))) {
                break;
            }
            if (file.frames.size() < kMaxFrames) {
                file.frames << line;
            }
            continue;
        }
        if (line == QLatin1String("frames:")) {
            inFrames = true;
            continue;
        }
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            file.fields.insert(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
        }
    }
    file.valid = file.fields.contains(QStringLiteral("signal"));
    return file;
}

QString sanitizeFrame(const QString &line)
{
    static const QRegularExpression withInfo(QStringLiteral(R"(^(.*)\(([^()]*)\)\s*(\[0x[0-9a-fA-F]+\])?$)"));
    static const QRegularExpression bare(QStringLiteral(R"(^(.*?)\s*\[0x[0-9a-fA-F]+\]$)"));
    static const QRegularExpression moduleName(QStringLiteral(R"(^[A-Za-z0-9._+-]{1,64}$)"));
    static const QRegularExpression symbol(QStringLiteral(R"(^[A-Za-z0-9_.$@]{0,256}(\+0x[0-9a-fA-F]{1,16})?$)"));

    QString module;
    QString inner;
    if (const auto m = withInfo.match(line); m.hasMatch()) {
        module = m.captured(1);
        inner = m.captured(2);
    } else if (const auto b = bare.match(line); b.hasMatch()) {
        module = b.captured(1);
    } else {
        return QStringLiteral("?");
    }
    module = module.mid(module.lastIndexOf(QLatin1Char('/')) + 1);
    if (!moduleName.match(module).hasMatch()) {
        module = QStringLiteral("?");
    }
    if (!symbol.match(inner).hasMatch()) {
        inner = QStringLiteral("?");
    }
    return inner.isEmpty() ? module : module + QLatin1Char(' ') + inner;
}

QString sanitizeValue(const QString &value)
{
    static const QRegularExpression outside(QStringLiteral(R"([^A-Za-z0-9 ._:+()/-])"));
    QString v = value;
    v.remove(outside);
    return v.trimmed().left(64);
}

QString signalName(int signal)
{
    switch (signal) {
    case SIGSEGV:
        return QStringLiteral("SIGSEGV");
    case SIGABRT:
        return QStringLiteral("SIGABRT");
    case SIGBUS:
        return QStringLiteral("SIGBUS");
    case SIGFPE:
        return QStringLiteral("SIGFPE");
    case SIGILL:
        return QStringLiteral("SIGILL");
    default:
        return QStringLiteral("signal %1").arg(signal);
    }
}

QStringList reportFields()
{
    QStringList keys;
    for (const auto &f : kFields) {
        keys << QString::fromLatin1(f.key);
    }
    return keys;
}

QJsonObject buildReport(const CrashFile &file, const QDate &date)
{
    QJsonObject sections[3];
    auto sectionFor = [&](const char *name) -> QJsonObject & {
        const QLatin1String n(name);
        return n == QLatin1String("app") ? sections[0] : n == QLatin1String("crash") ? sections[1] : sections[2];
    };
    for (const auto &f : kFields) {
        const QString key = QString::fromLatin1(f.key);
        const QString raw = file.fields.value(key);
        if (raw.isEmpty()) {
            continue;
        }
        QJsonObject &section = sectionFor(f.section);
        if (key == QLatin1String("signal")) {
            section.insert(key, signalName(raw.toInt()));
        } else if (key == QLatin1String("uptime")) {
            section.insert(QStringLiteral("uptime_seconds"), raw.toLongLong());
        } else if (key == QLatin1String("build_id")) {
            if (isHex(raw) && raw.size() <= 64) {
                section.insert(key, raw);
            }
        } else if (const QString v = sanitizeValue(raw); !v.isEmpty()) {
            section.insert(key, v);
        }
    }
    QJsonArray frames;
    for (const QString &line : file.frames) {
        frames.append(sanitizeFrame(line));
    }
    sections[1].insert(QStringLiteral("frames"), frames);

    return QJsonObject{
        {QStringLiteral("schema"), kSchema},
        {QStringLiteral("date"), date.toString(Qt::ISODate)},
        {QStringLiteral("app"), sections[0]},
        {QStringLiteral("crash"), sections[1]},
        {QStringLiteral("system"), sections[2]},
    };
}

QString osName(const QString &osRelease)
{
    QString name;
    QString version;
    for (const QString &line : osRelease.split(QLatin1Char('\n'))) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\'')))) {
            value = value.mid(1, value.size() - 2);
        }
        const QString key = line.left(eq).trimmed();
        if (key == QLatin1String("NAME")) {
            name = value;
        } else if (key == QLatin1String("VERSION_ID")) {
            version = value;
        }
    }
    return sanitizeValue(version.isEmpty() ? name : name + QLatin1Char(' ') + version);
}

} // namespace rostrum::crash
