import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Rostrum

// Mixer fader. Up/Down 1%, Page Up/Down 10%, M mute, S solo, 1/2/3 destination, double-click
// resets. The wheel adjusts it only when "scroll to adjust" is on.
QQC2.Slider {
    id: fader

    property string accessibleName
    property real resetValue: 1.0
    property bool dimmed: false

    signal edited(real value)
    signal muteRequested()
    signal soloRequested()
    signal destinationRequested(int index)

    from: 0
    to: 1
    // stepSize 0 keeps the desktop style from drawing tick marks; keys step by hand below.
    stepSize: 0
    snapMode: QQC2.Slider.NoSnap
    focusPolicy: Qt.StrongFocus
    wheelEnabled: Mixer.scrollToAdjust
    opacity: dimmed ? 0.45 : 1

    Accessible.name: accessibleName

    onMoved: edited(value)

    function nudge(delta) {
        edited(Math.max(from, Math.min(to, value + delta)))
    }

    Keys.onPressed: event => {
        switch (event.key) {
        case Qt.Key_Up:
        case Qt.Key_Right:
            nudge(0.01)
            break
        case Qt.Key_Down:
        case Qt.Key_Left:
            nudge(-0.01)
            break
        case Qt.Key_PageUp:
            nudge(0.1)
            break
        case Qt.Key_PageDown:
            nudge(-0.1)
            break
        case Qt.Key_M:
            muteRequested()
            break
        case Qt.Key_S:
            soloRequested()
            break
        case Qt.Key_1:
            destinationRequested(0)
            break
        case Qt.Key_2:
            destinationRequested(1)
            break
        case Qt.Key_3:
            destinationRequested(2)
            break
        default:
            return
        }
        event.accepted = true
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: fader.edited(fader.resetValue)
    }

    QQC2.ToolTip.visible: fader.visualFocus || (fader.hovered && !fader.pressed)
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    QQC2.ToolTip.text: i18nc("@info:tooltip", "%1\nArrows: 1%  Page Up/Down: 10%  Double-click: reset\nM: mute  S: solo  1/2/3: Phones/Stream/Both",
                             fader.accessibleName)
}
