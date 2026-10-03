import QtQuick
import QtQuick.Controls as QQC2

// Double-click to reset, for a Slider. A TapHandler on a Slider never sees the second click: the
// Slider grabs the mouse on press and cancels it. This watches the Slider's own presses instead,
// and fires after the Slider has applied the release, so the release cannot undo the reset.
QtObject {
    id: reset

    required property QQC2.Slider target
    signal triggered()

    property double lastPress: 0
    property real pressPosition: 0
    property bool armed: false

    // A second press that drags further than this is a drag, not a double-click.
    readonly property real dragTolerance: 0.03

    readonly property Connections watcher: Connections {
        target: reset.target
        function onPressedChanged() {
            const now = Date.now()
            if (reset.target.pressed) {
                reset.armed = now - reset.lastPress <= Qt.styleHints.mouseDoubleClickInterval
                reset.lastPress = reset.armed ? 0 : now
                reset.pressPosition = reset.target.position
            } else if (reset.armed) {
                reset.armed = false
                if (Math.abs(reset.target.position - reset.pressPosition) <= reset.dragTolerance) {
                    reset.triggered()
                }
            }
        }
    }
}
