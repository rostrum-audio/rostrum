import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// One running app: name and binary, its bus, a live meter, its own volume, Assign and Always.
// An app Rostrum placed by itself says so, and says why.
QQC2.Control {
    id: row

    required property var app
    readonly property bool assigned: app.busId !== ""

    padding: Kirigami.Units.largeSpacing
    Accessible.role: Accessible.ListItem
    Accessible.name: assigned ? i18nc("@info accessible: app on bus", "%1, on %2", app.name, app.busName)
                              : i18nc("@info accessible", "%1, unassigned", app.name)

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
        radius: Kirigami.Units.cornerRadius
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            Kirigami.Icon {
                source: row.app.icon || "application-x-executable"
                fallback: "application-x-executable"
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
                Accessible.ignored: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                QQC2.Label {
                    text: row.app.name
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                QQC2.Label {
                    // Why it is where it is: what Rostrum recognised, or how a rule matches.
                    text: {
                        const parts = []
                        if (row.app.binary) {
                            parts.push(row.app.binary)
                        }
                        if (row.app.detail) {
                            parts.push(row.app.detail)
                        } else {
                            parts.push(row.app.matchKey === "binary" ? i18nc("@info", "matched by binary")
                                                                     : i18nc("@info", "matched by name"))
                        }
                        if (Preferences.showNodeIds) {
                            parts.push(row.app.nodeIds.map(id => "#" + id).join(", "))
                        }
                        return parts.join(" · ")
                    }
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            QQC2.Label {
                visible: row.app.automatic
                text: i18nc("@info placed automatically", "Auto")
                font: Kirigami.Theme.smallFont
                opacity: 0.65
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Rostrum put it here. Move it, or turn on Always, to decide yourself.")
                QQC2.ToolTip.visible: autoHover.hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                HoverHandler {
                    id: autoHover
                }
            }

            // Bus chip, or "Unassigned". Text as well as color.
            QQC2.Label {
                text: row.assigned ? row.app.busName : i18nc("@info", "Unassigned")
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                font.weight: row.assigned ? Font.DemiBold : Font.Normal
                leftPadding: Kirigami.Units.smallSpacing * 2
                rightPadding: Kirigami.Units.smallSpacing * 2
                topPadding: 2
                bottomPadding: 2
                opacity: row.assigned ? 1 : 0.7
                background: Rectangle {
                    radius: height / 2
                    color: row.assigned ? Qt.alpha(row.app.busColor, 0.25) : "transparent"
                    border.width: 1
                    border.color: row.assigned ? row.app.busColor : Qt.alpha(Kirigami.Theme.textColor, 0.3)
                }
            }

            QQC2.Button {
                id: assignButton
                text: row.assigned ? i18nc("@action:button", "Move") : i18nc("@action:button", "Assign")
                icon.name: "go-next"
                onClicked: busMenu.popup(assignButton, 0, assignButton.height)
                Accessible.name: i18nc("@action:button accessible", "Assign %1 to a bus", row.app.name)
                BusMenu {
                    id: busMenu
                    currentBus: row.app.busId
                    onChosen: busId => Apps.assign(row.app.key, busId)
                    onUnassignRequested: Apps.unassign(row.app.key)
                }
            }
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            PeakMeter {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                orientation: Qt.Horizontal
                value: Apps.levels[row.app.key] ?? 0
                color: row.assigned ? row.app.busColor : Kirigami.Theme.positiveTextColor
                accessibleName: i18nc("@label accessible", "%1 level", row.app.name)
            }
            QQC2.ToolButton {
                icon.name: row.app.muted ? "audio-volume-muted" : "audio-volume-high"
                text: row.app.muted ? i18nc("@action:button", "Unmute") : i18nc("@action:button", "Mute")
                display: QQC2.AbstractButton.IconOnly
                checkable: true
                checked: row.app.muted
                onToggled: Apps.setMuted(row.app.key, checked)
                Accessible.name: i18nc("@action:button accessible", "Mute %1", row.app.name)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: !row.app.muted ? i18nc("@info:tooltip", "Mute this app")
                                 : row.app.always ? i18nc("@info:tooltip", "Muted. Saved with its rule in this scene.")
                                                  : i18nc("@info:tooltip", "Muted until Rostrum quits")
            }
            QQC2.Label {
                text: i18nc("@label per-app volume", "Volume")
                font: Kirigami.Theme.smallFont
                opacity: row.app.muted ? 0.4 : 0.7
            }
            PlainSlider {
                id: volume
                Layout.preferredWidth: Kirigami.Units.gridUnit * 6
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
            QQC2.Switch {
                text: i18nc("@option:check always send this app to its bus", "Always")
                enabled: row.assigned
                checked: row.app.always
                onToggled: Apps.setAlways(row.app.key, checked)
                Accessible.name: i18nc("@option:check accessible", "Always send %1 to %2", row.app.name, row.app.busName)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: checked ? i18nc("@info:tooltip", "New launches go to this bus too")
                                 : row.app.automatic ? i18nc("@info:tooltip", "Make it a rule, so it lands here even before Rostrum starts")
                                                     : i18nc("@info:tooltip", "Only this launch goes to this bus")
            }
        }
    }
}
