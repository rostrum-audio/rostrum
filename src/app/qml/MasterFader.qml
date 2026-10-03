import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// A master card: multiplies the bus sends, never replaces them. Leave iconName empty for the
// stream, which shows the on-air dot instead.
QQC2.Control {
    id: master

    property string label
    property string caption
    property string iconName
    property real value: 1
    property bool muted: false
    property real peak: 0
    property bool clip: false

    signal edited(real value)
    signal muteToggled()

    padding: Kirigami.Units.largeSpacing
    topPadding: Kirigami.Units.smallSpacing * 1.5
    bottomPadding: Kirigami.Units.largeSpacing

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius * 1.5
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing * 1.5

            Item {
                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                implicitHeight: implicitWidth
                Kirigami.Icon {
                    anchors.fill: parent
                    visible: master.iconName.length > 0
                    source: master.iconName
                    isMask: true
                    color: Kirigami.Theme.textColor
                }
                StreamDot {
                    anchors.centerIn: parent
                    visible: master.iconName.length === 0
                    size: Kirigami.Units.iconSizes.smallMedium * 0.6
                }
            }
            QQC2.Label {
                text: master.label
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            QQC2.Label {
                text: master.caption
                font: Kirigami.Theme.smallFont
                opacity: 0.55
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: master.muted ? i18nc("@info:status", "Muted") : Mixer.formatDb(master.value)
                font.features: { "tnum": 1 }
                font.weight: master.muted ? Font.Bold : Font.Normal
                color: master.muted ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                opacity: master.muted ? 1 : Mixer.showDb || slider.hovered || slider.activeFocus ? 0.75 : 0
            }
            QQC2.ToolButton {
                id: muteButton
                icon.name: master.muted ? "audio-volume-muted" : "audio-volume-high"
                icon.color: master.muted ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                text: master.muted ? i18nc("@action:button", "Unmute") : i18nc("@action:button", "Mute")
                display: QQC2.AbstractButton.IconOnly
                checkable: true
                checked: master.muted
                Accessible.name: i18nc("@action:button accessible", "%1 mute", master.label)
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: {
                    checked = Qt.binding(() => master.muted)
                    master.muteToggled()
                }
            }
        }

        PlainSlider {
            id: slider
            Layout.fillWidth: true
            from: 0
            to: 1
            value: master.value
            wheelEnabled: Mixer.scrollToAdjust
            opacity: master.muted ? 0.45 : 1
            focusPolicy: Qt.StrongFocus
            Accessible.name: i18nc("@label accessible", "%1 volume", master.label)
            onMoved: master.edited(value)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_PageUp) {
                    master.edited(Math.min(1, value + 0.1))
                } else if (event.key === Qt.Key_PageDown) {
                    master.edited(Math.max(0, value - 0.1))
                } else if (event.key === Qt.Key_M) {
                    master.muteToggled()
                } else {
                    return
                }
                event.accepted = true
            }
            SliderReset {
                target: slider
                onTriggered: master.edited(1.0)
            }
        }
        PeakMeter {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.smallSpacing
            orientation: Qt.Horizontal
            value: master.peak
            clip: master.clip
            opacity: master.muted ? 0.45 : 1
            accessibleName: i18nc("@label accessible", "%1 level", master.label)
        }
    }
}
