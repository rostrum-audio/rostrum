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
    property color accentColor: Kirigami.Theme.highlightColor
    readonly property real capHeight: Kirigami.Units.gridUnit * 1.1
    readonly property real capWidth: Kirigami.Units.gridUnit * 1.7
    readonly property real labelSpace: Kirigami.Units.gridUnit * 1.1
    readonly property real capX: Math.round((availableWidth - capWidth - labelSpace) / 2)
    // Gain is linear = position³, so a dB mark sits at cbrt(10^(dB/20)) along the travel.
    readonly property var scaleMarks: [6, 0, -6, -12, -24, -48]
        .map(db => ({ db: db, at: Math.cbrt(Math.pow(10, db / 20)) / to }))
        .filter(m => m.at <= 1)

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

    readonly property SliderReset doubleClickReset: SliderReset {
        target: fader
        onTriggered: fader.edited(fader.resetValue)
    }

    background: Item {
        x: fader.leftPadding
        y: fader.topPadding
        width: fader.availableWidth
        height: fader.availableHeight

        Rectangle {
            id: groove
            x: fader.capX + Math.round(fader.capWidth / 2 - width / 2)
            y: fader.capHeight / 2
            width: 4
            height: parent.height - fader.capHeight
            radius: 2
            color: Qt.alpha(Kirigami.Theme.textColor, 0.18)
        }

        Repeater {
            model: fader.scaleMarks
            Item {
                required property var modelData
                width: parent.width
                height: 1
                y: Math.round(groove.y + groove.height * (1 - modelData.at))

                Rectangle {
                    x: groove.x - fader.capWidth / 2 + 4
                    width: fader.capWidth / 2 - 8
                    height: 1
                    color: Qt.alpha(Kirigami.Theme.textColor, modelData.db === 0 ? 0.5 : 0.25)
                }
                Rectangle {
                    x: groove.x + groove.width + 4
                    width: fader.capWidth / 2 - 8
                    height: 1
                    color: Qt.alpha(Kirigami.Theme.textColor, modelData.db === 0 ? 0.5 : 0.25)
                }
                QQC2.Label {
                    x: groove.x + groove.width / 2 + fader.capWidth / 2 + 1
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.db > 0 ? "+" + modelData.db : modelData.db
                    font.pixelSize: Math.round(Kirigami.Units.gridUnit * 0.5)
                    font.features: { "tnum": 1 }
                    opacity: modelData.db === 0 ? 0.7 : 0.45
                }
            }
        }
    }

    handle: Rectangle {
        x: fader.leftPadding + fader.capX
        y: fader.topPadding + fader.visualPosition * (fader.availableHeight - height)
        width: fader.capWidth
        height: fader.capHeight
        radius: 3
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(Kirigami.Theme.alternateBackgroundColor, 1.9) }
            GradientStop { position: 1; color: Qt.lighter(Kirigami.Theme.alternateBackgroundColor, 1.35) }
        }
        border.width: fader.visualFocus ? 2 : 1
        border.color: fader.visualFocus || fader.pressed ? Kirigami.Theme.focusColor : Qt.alpha("black", 0.45)

        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 6
            height: 2
            radius: 1
            color: fader.accentColor
        }
    }

    QQC2.ToolTip.visible: fader.visualFocus || (fader.hovered && !fader.pressed)
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    QQC2.ToolTip.text: i18nc("@info:tooltip", "%1\nArrows: 1%  Page Up/Down: 10%  Double-click: reset\nM: mute  S: solo  1/2/3: Headphones/Stream/Both",
                             fader.accessibleName)
}
