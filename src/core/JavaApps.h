#pragma once

#include "core/Model.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace rostrum {

// A Java program Rostrum knows by name.
struct JavaApp
{
    QString name;
    AppCategory category = AppCategory::None;
};

// Whether the process is a Java virtual machine: every Java app reports the binary "java".
bool isJavaBinary(const QString &binary);

// The program a java command line runs, read the way the java launcher reads it: the main class,
// the jar's file name ("RuneLite.jar") or the module. Empty when there is none, or when an
// @argfile comes before it. commandLine[0] is the java executable.
QString javaProgram(const QStringList &commandLine);

// A known app, by main class, jar file name or the app's own name, in any case.
std::optional<JavaApp> knownJavaApp(const QString &programOrName);

// What a Java rule matches: the known app's name, so its launchers share one rule, else the
// program itself.
QString javaRuleMatch(const QString &program);

} // namespace rostrum
