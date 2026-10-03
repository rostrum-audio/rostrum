#pragma once

#include <QDate>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace rostrum::crash {

inline constexpr int kSchema = 1;
inline constexpr int kMaxFrames = 64;

// A crash file as the signal handler writes it: "key: value" lines, then "frames:" and one
// backtrace_symbols line per frame. It stays on this computer; only buildReport() output is sent.
struct CrashFile
{
    QMap<QString, QString> fields;
    QStringList frames;
    bool valid = false;
};

CrashFile parseCrashFile(const QByteArray &text);

// "module(symbol+0x1c) [0x7f…]" -> "module symbol+0x1c". The module keeps only its file name, the
// absolute address is dropped (it changes every run), and anything that does not look like a
// library name or a mangled symbol is replaced by "?". Returns "?" for lines that do not parse.
QString sanitizeFrame(const QString &line);

// Field values are versions and names such as "6.10.1" or "KDE". Anything outside that small
// alphabet is removed and the result is capped at 64 characters.
QString sanitizeValue(const QString &value);

QString signalName(int signal);

// The JSON that is uploaded. Only the fields listed in reportFields() are copied from the file.
QJsonObject buildReport(const CrashFile &file, const QDate &date);
QStringList reportFields();

// "NAME VERSION_ID" from /etc/os-release, e.g. "Ubuntu 26.04".
QString osName(const QString &osRelease);

} // namespace rostrum::crash
