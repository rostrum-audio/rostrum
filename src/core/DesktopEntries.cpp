#include "core/DesktopEntries.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace rostrum {

namespace {

constexpr qint64 kRescanAfterMs = 60 * 1000;

QString baseName(const QString &path)
{
    const auto slash = path.lastIndexOf(QLatin1Char('/'));
    return (slash >= 0 ? path.mid(slash + 1) : path).toLower();
}

bool isEnvAssignment(const QString &arg)
{
    const auto eq = arg.indexOf(QLatin1Char('='));
    return eq > 0 && !arg.left(eq).contains(QLatin1Char('/'));
}

} // namespace

QStringList execTokens(const QString &exec)
{
    const QStringList args = QProcess::splitCommand(exec);
    int i = 0;
    // env [-opts] [VAR=value]... program
    if (i < args.size() && baseName(args.at(i)) == QLatin1String("env")) {
        ++i;
        while (i < args.size() && (args.at(i).startsWith(QLatin1Char('-')) || isEnvAssignment(args.at(i)))) {
            ++i;
        }
    }
    while (i < args.size() && isEnvAssignment(args.at(i))) {
        ++i;
    }
    if (i >= args.size()) {
        return {};
    }
    const QString program = baseName(args.at(i));
    if (program == QLatin1String("flatpak")) {
        QStringList out;
        const auto run = args.indexOf(QStringLiteral("run"), i + 1);
        for (int k = run < 0 ? int(args.size()) : int(run) + 1; k < args.size(); ++k) {
            const QString &a = args.at(k);
            if (a.startsWith(QLatin1String("--command="))) {
                out << baseName(a.mid(int(qstrlen("--command="))));
            } else if (!a.startsWith(QLatin1Char('-'))) {
                out.prepend(a.toLower()); // the app id
                break;
            }
        }
        return out;
    }
    if ((program == QLatin1String("sh") || program == QLatin1String("bash")) && i + 2 < args.size() &&
        args.at(i + 1) == QLatin1String("-c")) {
        return execTokens(args.at(i + 2));
    }
    return {program};
}

std::optional<DesktopEntry> parseDesktopEntry(const QString &text, const QString &id)
{
    QHash<QString, QString> keys;
    bool inEntry = false;
    for (const QStringView raw : QStringView(text).split(QLatin1Char('\n'))) {
        const QStringView line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        const auto eq = line.indexOf(QLatin1Char('='));
        if (!inEntry || eq <= 0) {
            continue;
        }
        const QStringView key = line.left(eq).trimmed();
        if (key.contains(QLatin1Char('['))) {
            continue; // localized variants
        }
        keys.insert(key.toString(), line.mid(eq + 1).trimmed().toString());
    }
    const QString type = keys.value(QStringLiteral("Type"), QStringLiteral("Application"));
    if (type != QLatin1String("Application") || keys.value(QStringLiteral("Hidden")) == QLatin1String("true")) {
        return std::nullopt;
    }

    DesktopEntry e;
    e.id = id;
    e.name = keys.value(QStringLiteral("Name"));
    e.icon = keys.value(QStringLiteral("Icon"));
    e.categories = keys.value(QStringLiteral("Categories")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    e.tokens << id.toLower();
    e.tokens << execTokens(keys.value(QStringLiteral("Exec")));
    if (const QString tryExec = keys.value(QStringLiteral("TryExec")); !tryExec.isEmpty()) {
        e.tokens << baseName(tryExec);
    }
    if (const QString wmClass = keys.value(QStringLiteral("StartupWMClass")); !wmClass.isEmpty()) {
        e.tokens << wmClass.toLower();
    }
    if (const QString icon = keys.value(QStringLiteral("Icon")); !icon.isEmpty() && !icon.contains(QLatin1Char('/'))) {
        e.tokens << icon.toLower();
    }
    e.tokens.removeDuplicates();
    e.tokens.removeAll(QString());
    return e;
}

DesktopIndex::DesktopIndex(const QStringList &dataDirs)
    : m_dirs(dataDirs.isEmpty() ? defaultDataDirs() : dataDirs)
{
}

QStringList DesktopIndex::defaultDataDirs()
{
    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &extra : {QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share"),
                                 QStringLiteral("/var/lib/flatpak/exports/share"),
                                 QStringLiteral("/var/lib/snapd/desktop")}) {
        if (!dirs.contains(extra)) {
            dirs << extra;
        }
    }
    return dirs;
}

void DesktopIndex::scan()
{
    m_entries.clear();
    m_byToken.clear();
    QSet<QString> seenIds;
    for (const QString &dataDir : std::as_const(m_dirs)) {
        const QString root = dataDir + QStringLiteral("/applications");
        QDirIterator it(root, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QString id = QDir(root).relativeFilePath(path);
            id.chop(int(qstrlen(".desktop")));
            id.replace(QLatin1Char('/'), QLatin1Char('-'));
            // Earlier data dirs win, as in the XDG menu spec.
            if (seenIds.contains(id)) {
                continue;
            }
            seenIds.insert(id);
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                continue;
            }
            if (auto entry = parseDesktopEntry(QString::fromUtf8(f.readAll()), id)) {
                m_entries.append(*entry);
            }
        }
    }
    // Ids and programs first, so a stray Icon= or Name= never shadows another app's real id.
    for (int i = 0; i < m_entries.size(); ++i) {
        const QStringList &tokens = m_entries.at(i).tokens;
        for (int t = 0; t < tokens.size(); ++t) {
            if (!m_byToken.contains(tokens.at(t))) {
                m_byToken.insert(tokens.at(t), i);
            }
        }
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        const QString name = m_entries.at(i).name.toLower();
        if (!name.isEmpty() && !m_byToken.contains(name)) {
            m_byToken.insert(name, i);
        }
    }
    m_scanned.start();
}

std::optional<DesktopEntry> DesktopIndex::find(const QStringList &candidates)
{
    if (!m_scanned.isValid()) {
        scan();
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (const QString &c : candidates) {
            const QString key = c.trimmed().toLower();
            if (key.isEmpty()) {
                continue;
            }
            if (auto it = m_byToken.constFind(key); it != m_byToken.cend()) {
                return m_entries.at(it.value());
            }
        }
        if (pass == 0 && m_scanned.elapsed() > kRescanAfterMs) {
            scan();
            continue;
        }
        break;
    }
    return std::nullopt;
}

} // namespace rostrum
