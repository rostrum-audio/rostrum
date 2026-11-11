#pragma once

#include "core/Model.h"

#include <QString>

namespace rostrum::rule_export {

// Both fragments use "stream.rules": PipeWire matches them against each stream's own properties,
// in native clients (/usr/share/pipewire/client.conf) and in the streams pipewire-pulse creates
// for PulseAudio clients (pipewire-pulse.conf(5), STREAM RULES). "pulse.rules" match the client,
// which has no media.class, so they would also move the app's recording streams.
// WirePlumber 0.5 "stream.rules" only feed its restore logic and cannot route, so nothing
// goes into ~/.config/wireplumber/.
inline constexpr char kPulseRulesArray[] = "stream.rules";
inline constexpr char kClientRulesArray[] = "stream.rules";

// Every rule also matches this media.class, so it reaches only playback streams. An app's
// recording stream (Discord's mic, a browser call) must never be pointed at a bus.
inline constexpr char kPlaybackStreamClass[] = "Stream/Output/Audio";

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
