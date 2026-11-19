pragma Singleton
import QtQuick

// Fader.qml reads Mixer.scrollToAdjust. The quick tests do not load the app.
QtObject {
    property bool scrollToAdjust: false
}
