import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// The compact window: mic, scene, both masters and a slim row per bus, for keeping beside a game
// or OBS. Everything else is one click away in the full window.
QQC2.Pane {
    id: compact

    signal fullViewRequested(string page)

    padding: Kirigami.Units.smallSpacing * 1.5
    Kirigami.Theme.colorSet: Kirigami.Theme.Window

    component SlimRow: RowLayout {
        id: slimRow

        property string label
        property string iconName
        property color swatch: "transparent"
        property real value: 1
        property real maximum: 1
        property bool muted: false
        property bool dimmed: false

        signal edited(real value)
        signal muteToggled()

        spacing: Kirigami.Units.smallSpacing
        opacity: slimRow.dimmed ? 0.5 : 1

        Item {
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
            Kirigami.Icon {
                anchors.fill: parent
                visible: slimRow.iconName.length > 0
                source: slimRow.iconName
                isMask: true
                color: Kirigami.Theme.textColor
            }
            Rectangle {
                anchors.centerIn: parent
                visible: slimRow.iconName.length === 0
                width: parent.width * 0.6
                height: width
                radius: width / 2
                color: slimRow.swatch
            }
        }
        QQC2.Label {
            text: slimRow.label
            elide: Text.ElideRight
            Layout.preferredWidth: Kirigami.Units.gridUnit * 6
        }
        PlainSlider {
            id: slider
            Layout.fillWidth: true
            from: 0
            to: slimRow.maximum
            value: slimRow.value
            wheelEnabled: Mixer.scrollToAdjust
            opacity: slimRow.muted ? 0.45 : 1
            focusPolicy: Qt.StrongFocus
            Accessible.name: i18nc("@label accessible", "%1 volume", slimRow.label)
            onMoved: slimRow.edited(value)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_M) {
                    slimRow.muteToggled()
                    event.accepted = true
                }
            }
            SliderReset {
                target: slider
                onTriggered: slimRow.edited(1.0)
            }
        }
        QQC2.ToolButton {
            icon.name: slimRow.muted ? "audio-volume-muted" : "audio-volume-high"
            icon.color: slimRow.muted ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
            text: slimRow.muted ? i18nc("@action:button", "Unmute") : i18nc("@action:button", "Mute")
            display: QQC2.AbstractButton.IconOnly
            checkable: true
            checked: slimRow.muted
            Accessible.name: i18nc("@action:button accessible", "%1 mute", slimRow.label)
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            onClicked: {
                checked = Qt.binding(() => slimRow.muted)
                slimRow.muteToggled()
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            MicButton {
                implicitWidth: Math.max(Kirigami.Units.gridUnit * 6.5, implicitContentWidth + leftPadding + rightPadding)
                onDevicesRequested: compact.fullViewRequested("devices")
            }
            SceneSwitcher {
                enabled: App.connected
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                onSwitchRequested: name => applicationWindow().requestSceneSwitch(name)
                onManageRequested: compact.fullViewRequested("scenes")
            }
            QQC2.ToolButton {
                icon.name: "window-keep-above"
                text: i18nc("@action:button", "Keep on Top")
                display: QQC2.AbstractButton.IconOnly
                checkable: true
                checked: App.keepOnTop
                onToggled: App.keepOnTop = checked
                QQC2.ToolTip.text: i18n("Keep the compact window above other windows. Some Wayland desktops ignore this; on Plasma, use the window menu's Keep Above Others instead.")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                icon.name: "window-restore-pip"
                text: i18nc("@action:button", "Full View")
                display: QQC2.AbstractButton.IconOnly
                onClicked: compact.fullViewRequested("")
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !App.connected
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: "dialog-error"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: implicitWidth
            }
            QQC2.Label {
                text: i18n("Rostrum can't reach PipeWire.")
                color: Kirigami.Theme.negativeTextColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            QQC2.Button {
                text: i18nc("@action:button", "Details")
                onClicked: compact.fullViewRequested("")
            }
        }

        QQC2.ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            enabled: App.connected
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
            contentWidth: availableWidth

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 0

                SlimRow {
                    Layout.fillWidth: true
                    label: i18nc("@label master", "Headphones")
                    iconName: "audio-headphones"
                    value: Mixer.masterPhones
                    muted: Mixer.masterPhonesMuted
                    onEdited: v => Mixer.masterPhones = v
                    onMuteToggled: Mixer.masterPhonesMuted = !Mixer.masterPhonesMuted
                }
                SlimRow {
                    Layout.fillWidth: true
                    label: i18nc("@label master", "Stream")
                    iconName: "media-record"
                    value: Mixer.masterStream
                    muted: Mixer.masterStreamMuted
                    onEdited: v => Mixer.masterStream = v
                    onMuteToggled: Mixer.masterStreamMuted = !Mixer.masterStreamMuted
                }
                Kirigami.Separator {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                }
                Repeater {
                    model: Mixer.buses
                    delegate: SlimRow {
                        required property var model
                        Layout.fillWidth: true
                        label: model.name
                        swatch: model.busColor
                        value: model.volume
                        maximum: model.isInput ? 1.5 : 1.0
                        muted: model.muted
                        dimmed: model.dimmed
                        onEdited: v => Mixer.setVolume(model.busId, v)
                        onMuteToggled: Mixer.toggleMuted(model.busId)
                    }
                }
            }
        }
    }
}
