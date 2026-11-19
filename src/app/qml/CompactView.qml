import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// The compact window: mic, scene, both masters and a slim row per bus, for keeping beside a game
// or OBS. Each row has its name and state, a fader over its meter, the level, mute and, for a
// bus, where it goes. Everything else is one click away in the full window.
QQC2.Pane {
    id: compact

    signal fullViewRequested(string page)

    padding: Kirigami.Units.smallSpacing * 1.5
    Kirigami.Theme.colorSet: Kirigami.Theme.Window

    // Column widths shared by the header and every row.
    readonly property real nameWidth: Kirigami.Units.gridUnit * 6.5
    readonly property real levelWidth: Kirigami.Units.gridUnit * 3
    readonly property real routingWidth: Kirigami.Units.gridUnit * 6
    // Routing gives way first when the window is at its narrowest.
    readonly property bool showRouting: width >= Kirigami.Units.gridUnit * 26

    component SlimRow: RowLayout {
        id: slimRow

        property string label
        property string iconName
        property color swatch: "transparent"
        property real value: 1
        property real maximum: 1
        property bool muted: false
        property bool dimmed: false
        property bool soloed: false
        property real peak: 0
        property bool clip: false
        // -1 for a master, which has no destination.
        property int destination: -1

        signal edited(real value)
        signal muteToggled()
        signal destinationPicked(int index)

        readonly property string stateText: muted ? i18nc("@info:status", "Muted")
                                          : dimmed ? i18nc("@info:status", "Dimmed by solo")
                                          : soloed ? i18nc("@info:status", "Solo")
                                          : ""

        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.preferredWidth: compact.nameWidth
            Layout.maximumWidth: compact.nameWidth
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
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                QQC2.Label {
                    Layout.fillWidth: true
                    text: slimRow.label
                    elide: Text.ElideRight
                }
                // The state in words, so mute, solo and dimming never rest on colour or opacity alone.
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: slimRow.stateText
                    font: Kirigami.Theme.smallFont
                    elide: Text.ElideRight
                    color: slimRow.muted ? Kirigami.Theme.negativeTextColor
                         : slimRow.soloed ? Kirigami.Theme.neutralTextColor
                         : Kirigami.Theme.textColor
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            opacity: slimRow.dimmed ? 0.5 : 1
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
            PeakMeter {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.smallSpacing
                Layout.rightMargin: Kirigami.Units.smallSpacing
                orientation: Qt.Horizontal
                value: slimRow.peak
                clip: slimRow.clip
                opacity: slimRow.muted ? 0.45 : 1
                accessibleName: i18nc("@label accessible", "%1 level", slimRow.label)
            }
        }
        QQC2.Label {
            Layout.preferredWidth: compact.levelWidth
            horizontalAlignment: Text.AlignRight
            text: Mixer.formatDb(slimRow.value)
            font.features: { "tnum": 1 }
            opacity: slimRow.dimmed || slimRow.muted ? 0.5 : 0.8
            elide: Text.ElideRight
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
        Item {
            visible: compact.showRouting
            Layout.preferredWidth: compact.routingWidth
            implicitHeight: routing.implicitHeight
            DestinationControl {
                id: routing
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: slimRow.destination >= 0
                showCaption: false
                current: slimRow.destination
                busName: slimRow.label
                opacity: slimRow.dimmed ? 0.5 : 1
                onPicked: index => slimRow.destinationPicked(index)
            }
        }
    }

    component ColumnTitle: QQC2.Label {
        font: Kirigami.Theme.smallFont
        opacity: 0.7
        elide: Text.ElideRight
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
            QQC2.Button {
                icon.name: "window-restore-pip"
                text: i18nc("@action:button", "Full View")
                highlighted: true
                onClicked: compact.fullViewRequested("")
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
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    ColumnTitle {
                        Layout.preferredWidth: compact.nameWidth
                        text: i18nc("@title:column compact view", "Name")
                    }
                    ColumnTitle {
                        Layout.fillWidth: true
                        text: i18nc("@title:column compact view", "Level")
                    }
                    ColumnTitle {
                        text: i18nc("@title:column compact view", "Mute")
                    }
                    ColumnTitle {
                        visible: compact.showRouting
                        Layout.preferredWidth: compact.routingWidth
                        text: i18nc("@title:column compact view", "Routing")
                    }
                }

                SlimRow {
                    Layout.fillWidth: true
                    label: i18nc("@label master", "Headphones")
                    iconName: "audio-headphones"
                    value: Mixer.masterPhones
                    muted: Mixer.masterPhonesMuted
                    peak: Mixer.phonesPeak
                    clip: Mixer.phonesClip
                    onEdited: v => Mixer.masterPhones = v
                    onMuteToggled: Mixer.masterPhonesMuted = !Mixer.masterPhonesMuted
                }
                SlimRow {
                    Layout.fillWidth: true
                    label: i18nc("@label master", "Stream")
                    iconName: "media-record"
                    value: Mixer.masterStream
                    muted: Mixer.masterStreamMuted
                    peak: Mixer.streamPeak
                    clip: Mixer.streamClip
                    onEdited: v => Mixer.masterStream = v
                    onMuteToggled: Mixer.masterStreamMuted = !Mixer.masterStreamMuted
                }
                Kirigami.Separator {
                    Layout.fillWidth: true
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
                        soloed: model.soloed
                        peak: model.peak
                        clip: model.clip
                        destination: model.destination
                        onDestinationPicked: index => Mixer.setDestination(model.busId, index)
                        onEdited: v => model.isInput ? Mixer.setVolume(model.busId, v)
                                                   : Mixer.setVolumeTogether(model.busId, v)
                        onMuteToggled: Mixer.toggleMuted(model.busId)
                    }
                }
            }
        }
    }
}
