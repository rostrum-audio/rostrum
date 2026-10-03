#pragma once

#include "core/Model.h"

#include <QString>

namespace rostrum::rule_export {

// Array names copied from the files that read the fragments on this machine:
// /usr/share/pipewire/pipewire-pulse.conf ("pulse.rules", PulseAudio clients) and
// /usr/share/pipewire/client.conf ("stream.rules", native PipeWire streams).
// WirePlumber 0.5 "stream.rules" only feed its restore logic and cannot route, so nothing
// goes into ~/.config/wireplumber/.
inline constexpr char kPulseRulesArray[] = "pulse.rules";
inline constexpr char kClientRulesArray[] = "stream.rules";

// The only property Rostrum ever sets. node.dont-fallback, node.dont-move and
// node.dont-reconnect must never be written: when the bus node is missing, WirePlumber has to
// fall back to the default sink, or quitting Rostrum would silence every routed app.
inline constexpr char kTargetProperty[] = "target.object";

// "~^[Dd]iscord$"-style SPA regex that matches the whole value, ignoring ASCII case, like
// the in-app rule matching does.
QString caseInsensitiveRegex(const QString &value);

// Rules that can be exported: non-empty match, pointing at a playback bus of the scene.
// Binary rules come first so that name rules, applied later, win like they do in the app.
QList<AppRule> exportableRules(const Scene &scene);

// Fragment text. Empty when the scene has no exportable rules.
QString pulseFragment(const Scene &scene);
QString clientFragment(const Scene &scene);

// Writes both fragments, or removes them when there is nothing to export. Files are only
// touched when their content changes. Returns false and sets error on failure.
bool apply(const Scene &scene, const QString &pulsePath, const QString &clientPath, QString *error = nullptr);

} // namespace rostrum::rule_export
