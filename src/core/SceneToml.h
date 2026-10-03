#pragma once

#include "core/Model.h"

#include <QList>
#include <QString>

#include <optional>

namespace rostrum::toml_io {

inline constexpr int kFormatVersion = 1;

// One scene per file. Solo is not part of the format and is never written.
QString serializeScene(const Scene &scene);
std::optional<Scene> parseScene(const QString &text, QString *error = nullptr);

// Backup bundle: every scene in one file.
QString serializeBundle(const QList<Scene> &scenes);
QList<Scene> parseBundle(const QString &text, QString *error = nullptr);

// Makes a parsed scene safe to use: mic bus first, unique ids, at most kMaxBuses, valid colors,
// clamped levels, rules only for existing playback buses.
Scene sanitize(Scene scene);

} // namespace rostrum::toml_io
