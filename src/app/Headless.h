#pragma once

#include <QGuiApplication>

namespace rostrum::app {

// A test or screenshot run: Qt's offscreen platform, or ROSTRUM_SCREENSHOT naming a file. Such a
// run never uses the network, OBS, the tray, global shortcuts or notifications by itself.
inline bool headless()
{
    return QGuiApplication::platformName() == QLatin1String("offscreen") ||
           qEnvironmentVariable("QT_QPA_PLATFORM") == QLatin1String("offscreen") ||
           !qEnvironmentVariableIsEmpty("ROSTRUM_SCREENSHOT");
}

} // namespace rostrum::app
