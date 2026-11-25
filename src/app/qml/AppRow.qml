import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// One running app: name (+ detail when there is room), then fixed-width columns shared by
// every row (reason, meter, bus picker, Always, mute, volume) so controls line up.
// Meter uses the theme positive color; bus color is on the picker dot only.
// Unassign stays in the bus menu.
QQC2.Control {
    id: row

    required property var app
    readonly property bool assigned: app.busId !== ""
    readonly property string reasonText: {
        if (row.app.automatic) {
            return i18nc("@info placed automatically", "Auto")
        }
        if (row.app.always) {
            return i18nc("@info matched by a saved rule", "Rule")
        }
        if (row.assigned) {
            return i18nc("@info assigned by hand", "Manual")
        }
        return ""
    }
    readonly property string detailText: {
        const parts = []
        if (row.app.javaProgram || row.app.binary) {
            parts.push(row.app.javaProgram || row.app.binary)
        }
        if (row.app.detail) {
            parts.push(row.app.detail)
        } else {
            parts.push(row.app.matchKey === "binary" ? i18nc("@info", "matched by binary")
                       : row.app.matchKey === "java" ? i18nc("@info", "matched by Java program")
                       : i18nc("@info", "matched by name"))
        }
        if (Preferences.showNodeIds) {
            parts.push(row.app.nodeIds.map(id => "#" + id).join(", "))
        }
        return parts.join(" · ")
    }
    // Two lines when the row is wide enough for the name column minimum plus controls.
    readonly property bool showDetailLine: width >= Kirigami.Units.gridUnit * 28

    // Shared column widths (grid units) so meters and pickers align down the list.
    readonly property int reasonWidth: Kirigami.Units.gridUnit * 3
    readonly property int meterWidth: Kirigami.Units.gridUnit * 5
    readonly property int alwaysWidth: Kirigami.Units.gridUnit * 5
    readonly property int muteWidth: Kirigami.Units.gridUnit * 2
    readonly property int volumeWidth: Kirigami.Units.gridUnit * 4
    readonly property int busPickerInset: Kirigami.Units.largeSpacing
    readonly property int busPickerWidth: {
        const dot = Math.ceil(Kirigami.Units.iconSizes.small / 2)
        const arrow = Kirigami.Units.iconSizes.small
        const gap = Kirigami.Units.smallSpacing
        // Inset is applied inside contentItem (styles may ignore Button padding).
        const pad = row.busPickerInset * 2
        return Math.max(Kirigami.Units.gridUnit * 6,
                        pad + dot + gap + Math.ceil(longestBusMetrics.width) + gap + arrow)
    }

    TextMetrics {
        id: longestBusMetrics
        font: Kirigami.Theme.defaultFont
        text: {
            let best = i18nc("@info", "Unassigned")
            const buses = Apps.buses || []
            for (let i = 0; i < buses.length; ++i) {
                const n = buses[i].name || ""
                if (n.length > best.length) {
                    best = n
                }
            }
            return best
        }
    }

    padding: Kirigami.Units.smallSpacing
    leftPadding: Kirigami.Units.largeSpacing
    rightPadding: Kirigami.Units.largeSpacing
    Accessible.role: Accessible.ListItem
    Accessible.name: assigned ? i18nc("@info accessible: app on bus", "%1, on %2", app.name, app.busName)
                              : i18nc("@info accessible", "%1, unassigned", app.name)
    Accessible.description: row.detailText

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
        radius: Kirigami.Units.cornerRadius
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Icon {
                source: row.app.icon || "application-x-executable"
                fallback: "application-x-executable"
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Layout.alignment: Qt.AlignVCenter
                Accessible.ignored: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                Layout.alignment: Qt.AlignVCenter
                spacing: 0

                QQC2.Label {
                    text: row.app.name
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                QQC2.Label {
                    visible: row.showDetailLine && row.detailText.length > 0
                    text: row.detailText
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            // Fixed slot so empty reason still keeps columns aligned.
            Item {
                Layout.preferredWidth: row.reasonWidth
                Layout.minimumWidth: row.reasonWidth
                Layout.maximumWidth: row.reasonWidth
                Layout.fillWidth: false
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                QQC2.Label {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    visible: row.reasonText.length > 0
                    text: row.reasonText
                    font: Kirigami.Theme.smallFont
                    opacity: 0.65
                    elide: Text.ElideRight
                    QQC2.ToolTip.visible: reasonHover.hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: row.app.automatic
                        ? i18nc("@info:tooltip", "Rostrum put it here. Move it, or turn on Always, to decide yourself.")
                        : row.app.always
                            ? i18nc("@info:tooltip", "New launches go to this bus too")
                            : i18nc("@info:tooltip", "Only this launch goes to this bus")
                    HoverHandler {
                        id: reasonHover
                    }
                }
            }

            PeakMeter {
                Layout.preferredWidth: row.meterWidth
                Layout.minimumWidth: row.meterWidth
                Layout.maximumWidth: row.meterWidth
                Layout.fillWidth: false
                Layout.alignment: Qt.AlignVCenter
                orientation: Qt.Horizontal
                value: Apps.levels[row.app.key] ?? 0
                color: Kirigami.Theme.positiveTextColor
                accessibleName: i18nc("@label accessible", "%1 level", row.app.name)
            }

            QQC2.Button {
                id: busButton
                Layout.preferredWidth: row.busPickerWidth
                Layout.minimumWidth: row.busPickerWidth
                Layout.maximumWidth: row.busPickerWidth
                Layout.fillWidth: false
                Layout.alignment: Qt.AlignVCenter
                // Style may ignore padding with a custom contentItem; inset in the row instead.
                padding: 0
                Accessible.name: i18nc("@action:button accessible", "Assign %1 to a bus", row.app.name)
                onClicked: busMenu.popup(busButton, 0, busButton.height)
                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Item {
                        Layout.preferredWidth: row.busPickerInset
                        Layout.minimumWidth: row.busPickerInset
                    }
                    Rectangle {
                        visible: row.assigned
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small / 2
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small / 2
                        Layout.alignment: Qt.AlignVCenter
                        radius: width / 2
                        color: row.app.busColor
                    }
                    // Spacer when unassigned so the label still starts after the dot column.
                    Item {
                        visible: !row.assigned
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small / 2
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small / 2
                    }
                    QQC2.Label {
                        text: row.assigned ? row.app.busName : i18nc("@info", "Unassigned")
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        opacity: row.assigned ? 1 : 0.7
                    }
                    Kirigami.Icon {
                        source: "arrow-down"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        Layout.alignment: Qt.AlignVCenter
                        isMask: true
                        color: Kirigami.Theme.textColor
                        Accessible.ignored: true
                    }
                    Item {
                        Layout.preferredWidth: row.busPickerInset
                        Layout.minimumWidth: row.busPickerInset
                    }
                }
                BusMenu {
                    id: busMenu
                    currentBus: row.app.busId
                    onChosen: busId => Apps.assign(row.app.key, busId)
                    onUnassignRequested: Apps.unassign(row.app.key)
                }
            }

            QQC2.Switch {
                text: i18nc("@option:check always send this app to its bus", "Always")
                enabled: row.assigned
                checked: row.app.always
                onToggled: Apps.setAlways(row.app.key, checked)
                Layout.preferredWidth: row.alwaysWidth
                Layout.minimumWidth: row.alwaysWidth
                Layout.maximumWidth: row.alwaysWidth
                Layout.fillWidth: false
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: i18nc("@option:check accessible", "Always send %1 to %2", row.app.name, row.app.busName)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: checked ? i18nc("@info:tooltip", "New launches go to this bus too")
                                 : row.app.automatic ? i18nc("@info:tooltip", "Make it a rule, so it lands here even before Rostrum starts")
                                                     : i18nc("@info:tooltip", "Only this launch goes to this bus")
            }

            QQC2.ToolButton {
                icon.name: row.app.muted ? "audio-volume-muted" : "audio-volume-high"
                text: row.app.muted ? i18nc("@action:button", "Unmute") : i18nc("@action:button", "Mute")
                display: QQC2.AbstractButton.IconOnly
                checkable: true
                checked: row.app.muted
                onToggled: Apps.setMuted(row.app.key, checked)
                Layout.preferredWidth: row.muteWidth
                Layout.minimumWidth: row.muteWidth
                Layout.maximumWidth: row.muteWidth
                Layout.fillWidth: false
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: i18nc("@action:button accessible", "Mute %1", row.app.name)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: !row.app.muted ? i18nc("@info:tooltip", "Mute this app")
                                 : row.app.always ? i18nc("@info:tooltip", "Muted. Saved with its rule in this scene.")
                                                  : i18nc("@info:tooltip", "Muted until Rostrum quits")
            }

            PlainSlider {
                id: volume
                Layout.preferredWidth: row.volumeWidth
                Layout.minimumWidth: row.volumeWidth
                Layout.maximumWidth: row.volumeWidth
                Layout.fillWidth: false
                Layout.alignment: Qt.AlignVCenter
                opacity: row.app.muted ? 0.5 : 1
                from: 0
                to: 1
                value: row.app.volume
                wheelEnabled: Preferences.scrollToAdjust
                onMoved: Apps.setVolume(row.app.key, value)
                Accessible.name: i18nc("@label accessible", "%1 volume", row.app.name)
                QQC2.ToolTip.visible: hovered || pressed || visualFocus
                QQC2.ToolTip.text: i18nc("@info:tooltip percent", "%1%", Math.round(value * 100))
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: (row.app.divertedTo ?? "") !== ""
            type: Kirigami.MessageType.Warning
            text: i18nc("@info %1 is a device or program, e.g. Easy Effects Sink, %2 a bus",
                        "Another program moved it to %1, so it plays there instead of on %2. Its bus meter, fader and mute do not reach it.",
                        row.app.divertedTo ?? "", row.app.busName)
            actions: Kirigami.Action {
                text: i18nc("@action:button", "Move Back")
                icon.name: "go-previous"
                onTriggered: Mixer.reclaimStreams(row.app.divertedIds)
            }
        }
    }
}
