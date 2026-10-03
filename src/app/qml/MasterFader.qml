import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Horizontal master: multiplies the bus sends, never replaces them.
RowLayout {
    id: master

    property string label
    property string iconName
    property real value: 1
    property bool muted: false
    property real peak: 0
    property bool clip: false

    signal edited(real value)
    signal muteToggled()

    spacing: Kirigami.Units.smallSpacing

    Kirigami.Icon {
        source: master.iconName
        implicitWidth: Kirigami.Units.iconSizes.smallMedium
        implicitHeight: implicitWidth
    }
    QQC2.Label {
        text: master.label
        font.weight: Font.DemiBold
        Layout.preferredWidth: Kirigami.Units.gridUnit * 4
        elide: Text.ElideRight
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
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
            TapHandler {
                onDoubleTapped: master.edited(1.0)
            }
        }
        PeakMeter {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.smallSpacing
            orientation: Qt.Horizontal
            value: master.peak
            clip: master.clip
            accessibleName: i18nc("@label accessible", "%1 level", master.label)
        }
    }

    QQC2.Label {
        text: Mixer.formatDb(master.value)
        opacity: 0.7
        visible: Mixer.showDb || slider.hovered || slider.activeFocus
        Layout.preferredWidth: Kirigami.Units.gridUnit * 3.5
        horizontalAlignment: Text.AlignRight
    }

    QQC2.Button {
        text: master.muted ? i18nc("@action:button", "Muted") : i18nc("@action:button", "Mute")
        icon.name: master.muted ? "audio-volume-muted" : "audio-volume-high"
        checkable: true
        checked: master.muted
        Accessible.name: i18nc("@action:button accessible", "%1 mute", master.label)
        onClicked: {
            checked = Qt.binding(() => master.muted)
            master.muteToggled()
        }
    }
}
