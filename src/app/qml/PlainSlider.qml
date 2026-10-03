import QtQuick
import QtQuick.Controls as QQC2

// A slider without tick marks. The desktop style draws ticks whenever stepSize is set, so
// stepSize stays 0 and the arrow keys step by `keyStep` here instead.
QQC2.Slider {
    id: slider

    property real keyStep: 0.01

    stepSize: 0
    snapMode: QQC2.Slider.NoSnap

    function nudge(delta) {
        const v = Math.max(from, Math.min(to, value + delta))
        if (v !== value) {
            value = v
            moved()
        }
    }

    Keys.onPressed: event => {
        const forward = orientation === Qt.Horizontal ? Qt.Key_Right : Qt.Key_Up
        const back = orientation === Qt.Horizontal ? Qt.Key_Left : Qt.Key_Down
        if (event.key === forward) {
            nudge(keyStep)
            event.accepted = true
        } else if (event.key === back) {
            nudge(-keyStep)
            event.accepted = true
        }
    }
}
