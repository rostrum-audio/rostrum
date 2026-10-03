import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// One bus: color bar and name, meter and fader, M/S, destination, app chips. The mic strip
// shows gain instead, takes no app drops and hides sidetone behind an expander.
QQC2.Control {
    id: strip

    required property int index
    required property string busId
    required property string name
    required property string busColor
    required property bool isInput
    required property real volume
    required property bool muted
    required property bool soloed
    required property bool dimmed
    required property int destination
    required property var apps
    required property real peak
    required property bool clip

    property bool editing: false
    property bool expanded: false
    readonly property bool dropHover: dropArea.containsDrag
    readonly property int maxChips: 3

    signal removeRequested(string busId, string name)
    signal overflowRequested(string busId)

    function startRename() {
        editing = true
    }

    implicitWidth: Kirigami.Units.gridUnit * 8
    leftPadding: Kirigami.Units.smallSpacing + 4 + Kirigami.Units.smallSpacing
    rightPadding: Kirigami.Units.smallSpacing
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius
        color: strip.isInput ? Qt.alpha(strip.busColor, 0.06) : Kirigami.Theme.backgroundColor
        border.width: strip.dropHover ? 2 : 1
        border.color: strip.dropHover ? Kirigami.Theme.highlightColor : Qt.alpha(Kirigami.Theme.textColor, 0.15)

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: 4
            radius: 2
            color: strip.busColor
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        // Name: click to rename. Enter commits, Escape cancels.
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                visible: strip.isInput
                source: "audio-input-microphone"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: implicitWidth
            }
            QQC2.Label {
                visible: !strip.editing
                text: strip.name
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.fillWidth: true
                Accessible.role: Accessible.Heading
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Click to rename")
                QQC2.ToolTip.visible: nameHover.hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                HoverHandler {
                    id: nameHover
                    cursorShape: Qt.IBeamCursor
                }
                TapHandler {
                    onTapped: strip.startRename()
                }
            }
            QQC2.TextField {
                id: nameField
                visible: strip.editing
                Layout.fillWidth: true
                text: strip.name
                maximumLength: 40
                Accessible.name: i18nc("@label accessible", "%1 name", strip.name)
                onVisibleChanged: if (visible) {
                    text = strip.name
                    selectAll()
                    forceActiveFocus()
                }
                onAccepted: {
                    Mixer.rename(strip.busId, text)
                    strip.editing = false
                }
                Keys.onEscapePressed: strip.editing = false
                onActiveFocusChanged: if (!activeFocus) strip.editing = false
            }
        }

        // State chips keep their row so strips do not jump.
        Item {
            Layout.fillWidth: true
            implicitHeight: stateLabel.implicitHeight
            QQC2.Label {
                id: stateLabel
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                text: strip.muted ? i18nc("@info:status", "Muted")
                    : strip.dimmed ? i18nc("@info:status", "Dimmed by solo")
                    : strip.soloed ? i18nc("@info:status", "Solo")
                    : strip.isInput ? i18nc("@label", "Gain")
                    : ""
                color: strip.muted ? Kirigami.Theme.negativeTextColor
                     : strip.soloed ? Kirigami.Theme.neutralTextColor
                     : Kirigami.Theme.disabledTextColor
                font.weight: strip.muted || strip.soloed ? Font.Bold : Font.Normal
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Kirigami.Units.smallSpacing

            Item {
                Layout.fillWidth: true
            }
            PeakMeter {
                Layout.fillHeight: true
                value: strip.peak
                clip: strip.clip
                color: strip.busColor
                accessibleName: i18nc("@label accessible", "%1 level", strip.name)
            }
            Fader {
                id: fader
                Layout.fillHeight: true
                Layout.preferredWidth: 48
                Layout.minimumHeight: Kirigami.Units.gridUnit * 4
                orientation: Qt.Vertical
                to: strip.isInput ? 1.5 : 1.0
                value: strip.volume
                dimmed: strip.muted || strip.dimmed
                accessibleName: strip.isInput ? i18nc("@label accessible", "%1 gain", strip.name)
                                              : i18nc("@label accessible", "%1 volume", strip.name)
                onEdited: v => Mixer.setVolume(strip.busId, v)
                onMuteRequested: Mixer.toggleMuted(strip.busId)
                onSoloRequested: if (!strip.isInput) Mixer.toggleSolo(strip.busId)
                onDestinationRequested: i => Mixer.setDestination(strip.busId, i)
                Keys.onMenuPressed: contextMenu.popup(fader, 0, 0)
                Keys.onReleased: event => {
                    if (event.key === Qt.Key_F2) {
                        strip.startRename()
                        event.accepted = true
                    }
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }

        QQC2.Label {
            Layout.alignment: Qt.AlignHCenter
            text: Mixer.formatDb(strip.volume)
            font: Kirigami.Theme.smallFont
            opacity: (Mixer.showDb || fader.hovered || fader.activeFocus) ? 0.8 : 0
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.Button {
                Layout.fillWidth: true
                text: i18nc("@action:button short for mute", "M")
                checkable: true
                checked: strip.muted
                Accessible.name: i18nc("@action:button accessible", "%1 mute", strip.name)
                QQC2.ToolTip.text: strip.muted ? i18nc("@info:tooltip", "Unmute (M)") : i18nc("@info:tooltip", "Mute (M)")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: {
                    checked = Qt.binding(() => strip.muted)
                    Mixer.toggleMuted(strip.busId)
                }
            }
            QQC2.Button {
                Layout.fillWidth: true
                visible: !strip.isInput
                text: i18nc("@action:button short for solo", "S")
                checkable: true
                checked: strip.soloed
                Accessible.name: i18nc("@action:button accessible", "%1 solo", strip.name)
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Solo (S). Not saved in the scene.")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: {
                    checked = Qt.binding(() => strip.soloed)
                    Mixer.toggleSolo(strip.busId)
                }
            }
        }

        DestinationControl {
            Layout.fillWidth: true
            current: strip.destination
            busName: strip.name
            onPicked: i => Mixer.setDestination(strip.busId, i)
        }

        // Playback: up to three chips, then "+N".
        ColumnLayout {
            visible: !strip.isInput
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 4.6
            spacing: 2

            Repeater {
                model: strip.apps.slice(0, strip.maxChips)
                AppChip {
                    required property var modelData
                    Layout.fillWidth: true
                    appKey: modelData.key
                    appName: modelData.name
                    busId: strip.busId
                    busColor: strip.busColor
                }
            }
            QQC2.Button {
                visible: strip.apps.length > strip.maxChips
                flat: true
                font: Kirigami.Theme.smallFont
                text: i18nc("@action:button more apps", "+%1 more", strip.apps.length - strip.maxChips)
                onClicked: strip.overflowRequested(strip.busId)
            }
            QQC2.Label {
                visible: strip.apps.length === 0
                Layout.fillWidth: true
                text: i18nc("@info placeholder", "Drop an app here")
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
            Item {
                Layout.fillHeight: true
            }
        }

        // Mic: sidetone is a small secondary fader, collapsed by default.
        ColumnLayout {
            visible: strip.isInput
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 4.6
            spacing: 2

            QQC2.ToolButton {
                Layout.fillWidth: true
                text: i18nc("@action:button", "Sidetone")
                icon.name: strip.expanded ? "arrow-up" : "arrow-down"
                font: Kirigami.Theme.smallFont
                checkable: true
                checked: strip.expanded
                onToggled: strip.expanded = checked
                Accessible.name: i18nc("@action:button accessible", "Show sidetone")
            }
            QQC2.Switch {
                visible: strip.expanded
                text: i18nc("@option:check", "On")
                font: Kirigami.Theme.smallFont
                checked: App.sidetoneEnabled
                onToggled: App.sidetoneEnabled = checked
                Accessible.name: i18nc("@option:check accessible", "Sidetone")
            }
            PlainSlider {
                visible: strip.expanded
                Layout.fillWidth: true
                from: 0
                to: 1
                value: App.sidetoneVolume
                enabled: App.sidetoneEnabled
                wheelEnabled: Mixer.scrollToAdjust
                Accessible.name: i18nc("@label accessible", "Sidetone volume")
                onMoved: App.sidetoneVolume = value
            }
            Item {
                Layout.fillHeight: true
            }
        }
    }

    DropArea {
        id: dropArea
        anchors.fill: parent
        enabled: !strip.isInput
        keys: ["application/x-rostrum-app"]
        onDropped: drop => {
            const key = drop.getDataAsString("application/x-rostrum-app")
            if (key) {
                Mixer.assignApp(key, strip.busId)
                drop.acceptProposedAction()
            }
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }

    QQC2.Menu {
        id: contextMenu
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Rename")
            icon.name: "edit-rename"
            onTriggered: strip.startRename()
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Color…")
            icon.name: "color-management"
            onTriggered: colorPopup.open()
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Duplicate")
            icon.name: "edit-copy"
            visible: !strip.isInput
            enabled: Mixer.canAddBus
            height: visible ? implicitHeight : 0
            onTriggered: Mixer.duplicate(strip.busId)
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Remove…")
            icon.name: "edit-delete"
            visible: !strip.isInput
            height: visible ? implicitHeight : 0
            onTriggered: strip.removeRequested(strip.busId, strip.name)
        }
    }

    ColorPopup {
        id: colorPopup
        current: strip.busColor
        x: (strip.width - width) / 2
        y: Kirigami.Units.gridUnit * 2
        onPicked: color => Mixer.recolor(strip.busId, color)
    }
}
