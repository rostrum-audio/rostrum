#pragma once

#include <QString>

// Locating librostrum-dsp, the LADSPA plugin the PipeWire daemon loads for mic filters.
namespace rostrum::dsp {

// This build has the plugin, and whether it was built with RNNoise.
bool built();
bool hasDenoise();

// The plugin file for PipeWire to load, or empty with *error set. Checks ROSTRUM_DSP_PLUGIN,
// then the build tree (for a run from it), then next to the executable, then the install path.
// From an AppImage the file is first copied out of the mount, which goes away on quit while
// PipeWire still has the plugin loaded.
QString pluginPath(QString *error = nullptr);

// Copies source to dir/<content hash>/<file name> unless an identical copy is there, writing
// through a temporary file so a loaded copy is never changed in place. Removes copies from
// older versions. Returns the copy's path, or empty with *error set.
QString stableCopy(const QString &source, const QString &dir, QString *error = nullptr);

} // namespace rostrum::dsp
