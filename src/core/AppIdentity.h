#pragma once

#include "core/Model.h"

#include <QList>
#include <QString>

namespace rostrum {

// The PipeWire properties Rostrum uses to recognise an app.
struct StreamProps
{
    QString appName;   // application.name
    QString binary;    // application.process.binary
    QString mediaName; // media.name
    QString nodeName;  // node.name
    // What a Java rule matches (core/JavaApps.h): the known app's name or the program a Java
    // playback stream's process runs. Empty for every other stream, and when it cannot be read.
    QString javaProgram;
    // The command-line program before known-name normalisation; empty when it matches javaProgram.
    QString javaRaw;
};

struct AppKey
{
    MatchKey key = MatchKey::Name;
    QString match;

    QString toString() const; // "name:Discord" / "binary:discord"
    static AppKey fromString(const QString &s);
    bool isValid() const { return !match.isEmpty(); }
    bool operator==(const AppKey &o) const
    {
        return key == o.key && match.compare(o.match, Qt::CaseInsensitive) == 0;
    }
};

struct AppIdentity
{
    QString displayName; // human name shown in the UI
    QString binary;      // cleaned binary, may be empty
    AppKey key;          // what a new rule for this app matches on
    bool unnamed = false; // producing audio without a usable application.name
    QString javaProgram;  // set when the identity is a Java app's program (rule match)
    QString javaRaw;      // command-line program when it differs from javaProgram (UI detail)
};

// Strips " (deleted)" (left behind when a binary is updated while running) and paths.
QString cleanBinary(const QString &binary);

// application.name values that do not identify the app (Chromium/WebRTC, ALSA shims, ...).
bool isGenericAppName(const QString &name);

AppIdentity identify(const StreamProps &props);

// First rule that matches: name rules against application.name, then Java rules against the
// program a Java app runs, then binary rules against application.process.binary. Returns -1 if
// none.
int matchRule(const QList<AppRule> &rules, const StreamProps &props);

bool ruleMatches(const AppRule &rule, const StreamProps &props);

} // namespace rostrum
