import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// One bus: color band and name, meter and fader, M/S, destination, app chips. The mic strip
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
    required property string autoCategory
    required property real balance
    required property bool ducked

    property bool editing: false
    property bool expanded: false
    readonly property bool dropHover: dropArea.containsDrag
    readonly property int maxChips: 3
    readonly property var autoLabels: ({
        game: i18nc("@info bus receives automatically", "Auto: games"),
        voice: i18nc("@info bus receives automatically", "Auto: voice chat"),
        music: i18nc("@info bus receives automatically", "Auto: music"),
        alerts: i18nc("@info bus receives automatically", "Auto: stream alerts"),
        desktop: i18nc("@info bus receives automatically", "Auto: everything else")
    })

    readonly property string balanceText: balance === 0 ? i18nc("@info balance", "Centre")
        : balance < 0 ? i18nc("@info balance", "Left %1%", Math.round(-balance * 100))
        : i18nc("@info balance", "Right %1%", Math.round(balance * 100))

    signal removeRequested(string busId, string name)
    signal overflowRequested(string busId)

    function startRename() {
        editing = true
    }

    implicitWidth: Kirigami.Units.gridUnit * 8
    leftPadding: Kirigami.Units.smallSpacing * 2
    rightPadding: Kirigami.Units.smallSpacing * 2
    topPadding: Kirigami.Units.smallSpacing * 2 + 3
    bottomPadding: Kirigami.Units.smallSpacing * 2

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius * 1.5
        color: strip.isInput ? Qt.tint(Kirigami.Theme.backgroundColor, Qt.alpha(strip.busColor, 0.05))
                             : Kirigami.Theme.backgroundColor
        border.width: strip.dropHover ? 2 : 1
        border.color: strip.dropHover ? Kirigami.Theme.highlightColor : Qt.alpha(Kirigami.Theme.textColor, 0.12)

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: 3
            topLeftRadius: Kirigami.Units.cornerRadius * 1.5 - 1
            topRightRadius: Kirigami.Units.cornerRadius * 1.5 - 1
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
                isMask: true
                color: Kirigami.Theme.textColor
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
                font: Kirigami.Theme.smallFont
                text: strip.muted ? i18nc("@info:status", "Muted")
                    : strip.dimmed ? i18nc("@info:status", "Dimmed by solo")
                    : strip.soloed ? i18nc("@info:status", "Solo")
                    : strip.ducked ? i18nc("@info:status turned down while someone speaks", "Ducked")
                    : strip.isInput ? i18nc("@label", "Gain")
                    : Preferences.autoAssign ? (strip.autoLabels[strip.autoCategory] ?? "")
                    : ""
                color: strip.muted ? Kirigami.Theme.negativeTextColor
                     : strip.soloed ? Kirigami.Theme.neutralTextColor
                     : strip.ducked ? Kirigami.Theme.activeTextColor
                     : Kirigami.Theme.disabledTextColor
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
                id: meter
                Layout.fillHeight: true
                // Lines the lit track up with the fader groove; the clip mark sits just above it.
                Layout.topMargin: Math.max(0, fader.topPadding + fader.capHeight / 2 - meter.clipSize - 1)
                Layout.bottomMargin: fader.bottomPadding + fader.capHeight / 2
                value: strip.peak
                clip: strip.clip
                color: strip.busColor
                accessibleName: i18nc("@label accessible", "%1 level", strip.name)
            }
            Fader {
                id: fader
                Layout.fillHeight: true
                Layout.preferredWidth: Kirigami.Units.gridUnit * 3.2
                Layout.minimumHeight: Kirigami.Units.gridUnit * 4
                orientation: Qt.Vertical
                accentColor: strip.busColor
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
            font.features: { "tnum": 1 }
            opacity: (Mixer.showDb || fader.hovered || fader.activeFocus) ? 0.8 : 0
        }

        // The mic strip keeps the row empty so every fader has the same height.
        Item {
            Layout.fillWidth: true
            implicitHeight: balanceSlider.implicitHeight
            PlainSlider {
                id: balanceSlider
                anchors.left: parent.left
                anchors.right: parent.right
                visible: !strip.isInput
                from: -1
                to: 1
                keyStep: 0.05
                value: strip.balance
                wheelEnabled: Mixer.scrollToAdjust
                Accessible.name: i18nc("@label accessible", "%1 balance", strip.name)
                Accessible.description: strip.balanceText
                QQC2.ToolTip.visible: hovered || pressed
                QQC2.ToolTip.delay: pressed ? 0 : Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Balance: %1. Double-click to centre.", strip.balanceText)
                onMoved: Mixer.setBalance(strip.busId, value)
                Keys.onMenuPressed: contextMenu.popup(balanceSlider, 0, 0)
                TapHandler {
                    onDoubleTapped: Mixer.setBalance(strip.busId, 0)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            StripToggle {
                Layout.fillWidth: true
                text: i18nc("@action:button short for mute", "M")
                on: strip.muted
                activeColor: Kirigami.Theme.negativeTextColor
                Accessible.name: i18nc("@action:button accessible", "%1 mute", strip.name)
                QQC2.ToolTip.text: strip.muted ? i18nc("@info:tooltip", "Unmute (M)") : i18nc("@info:tooltip", "Mute (M)")
                onClicked: Mixer.toggleMuted(strip.busId)
            }
            StripToggle {
                Layout.fillWidth: true
                visible: !strip.isInput
                text: i18nc("@action:button short for solo", "S")
                on: strip.soloed
                activeColor: Kirigami.Theme.neutralTextColor
                Accessible.name: i18nc("@action:button accessible", "%1 solo", strip.name)
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Solo (S). Not saved in the scene.")
                onClicked: Mixer.toggleSolo(strip.busId)
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
                    appIcon: modelData.icon
                    automatic: modelData.automatic
                    reason: modelData.reason
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
            Rectangle {
                visible: strip.apps.length === 0
                Layout.fillWidth: true
                implicitHeight: Kirigami.Units.gridUnit * 2.2
                radius: Kirigami.Units.cornerRadius
                color: strip.dropHover ? Qt.alpha(Kirigami.Theme.highlightColor, 0.12) : "transparent"
                border.width: 1
                border.color: strip.dropHover ? Kirigami.Theme.highlightColor : Qt.alpha(Kirigami.Theme.textColor, 0.15)
                QQC2.Label {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    text: i18nc("@info placeholder", "Drop an app here")
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                }
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

            QQC2.AbstractButton {
                id: sidetoneToggle
                Layout.fillWidth: true
                text: i18nc("@action:button", "Sidetone")
                checkable: true
                checked: strip.expanded
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                padding: Kirigami.Units.smallSpacing
                onToggled: strip.expanded = checked
                Accessible.name: i18nc("@action:button accessible", "Show sidetone")
                background: Rectangle {
                    radius: Kirigami.Units.cornerRadius
                    color: sidetoneToggle.down ? Qt.alpha(Kirigami.Theme.textColor, 0.16)
                         : sidetoneToggle.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.1)
                         : "transparent"
                    border.width: sidetoneToggle.visualFocus ? 2 : 0
                    border.color: Kirigami.Theme.focusColor
                }
                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Chevron {
                        Layout.alignment: Qt.AlignVCenter
                        up: strip.expanded
                        opacity: 0.75
                    }
                    QQC2.Label {
                        text: sidetoneToggle.text
                        font: Kirigami.Theme.smallFont
                        Layout.fillWidth: true
                    }
                    QQC2.Label {
                        text: App.sidetoneEnabled ? i18nc("@info:status sidetone", "On") : i18nc("@info:status sidetone", "Off")
                        font: Kirigami.Theme.smallFont
                        opacity: 0.6
                    }
                }
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
        QQC2.Menu {
            id: autoMenu
            title: i18nc("@title:menu", "Receives Automatically")
            enabled: !strip.isInput
            Instantiator {
                model: [
                    { id: "game", text: i18nc("@item:inmenu automatic bus", "Games") },
                    { id: "voice", text: i18nc("@item:inmenu automatic bus", "Voice Chat") },
                    { id: "music", text: i18nc("@item:inmenu automatic bus", "Music Players") },
                    { id: "alerts", text: i18nc("@item:inmenu automatic bus", "Stream Alerts") },
                    { id: "desktop", text: i18nc("@item:inmenu automatic bus", "Everything Else") },
                    { id: "none", text: i18nc("@item:inmenu automatic bus", "Nothing") }
                ]
                QQC2.MenuItem {
                    required property var modelData
                    text: modelData.text
                    checkable: true
                    checked: strip.autoCategory === modelData.id
                    onTriggered: Mixer.setAutoCategory(strip.busId, modelData.id)
                }
                onObjectAdded: (index, object) => autoMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => autoMenu.removeItem(object)
            }
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Centre Balance")
            icon.name: "format-justify-center"
            visible: !strip.isInput
            enabled: strip.balance !== 0
            height: visible ? implicitHeight : 0
            onTriggered: Mixer.setBalance(strip.busId, 0)
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
