import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// An assigned app. Drag it to another strip, or onto the mixer background to unassign.
// Keyboard: the menu button offers "Move to" and "Unassign".
QQC2.Control {
    id: chip

    required property string appKey
    required property string appName
    property string appIcon
    property string busId
    property color busColor: Kirigami.Theme.highlightColor
    property bool automatic: false
    property string reason
    // Another program moved the app off this bus: where it plays instead, and its streams.
    property string divertedTo
    property var divertedIds: []
    readonly property bool diverted: divertedTo.length > 0
    readonly property string divertedText: i18nc("@info %1 is a device or program, e.g. Easy Effects Sink",
                                                 "Not on this bus: another program moved it to %1, so this bus's meter, fader and mute do not reach it.",
                                                 divertedTo)

    readonly property string mimeType: "application/x-rostrum-app"

    implicitHeight: Kirigami.Units.gridUnit * 1.4
    leftPadding: Kirigami.Units.smallSpacing * 2
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true

    Accessible.role: Accessible.StaticText
    Accessible.name: appName
    Accessible.description: [diverted ? divertedText : "",
                             automatic ? i18nc("@info accessible", "Placed automatically. %1", reason) : ""]
        .filter(line => line.length > 0).join(" ")

    QQC2.ToolTip.text: [nameLabel.truncated ? appName : "",
                        diverted ? divertedText : "",
                        automatic ? i18nc("@info:tooltip %1 is why", "Placed automatically: %1", reason) : ""]
        .filter(line => line.length > 0).join("\n")
    QQC2.ToolTip.visible: hovered && (automatic || diverted || nameLabel.truncated)
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

    background: Rectangle {
        radius: height / 2
        color: Qt.alpha(chip.busColor, chip.hovered ? 0.35 : 0.22)
        border.color: chip.diverted ? Kirigami.Theme.neutralTextColor : Qt.alpha(chip.busColor, 0.6)
        border.width: chip.diverted ? 2 : 1
    }

    contentItem: RowLayout {
        spacing: 0
        Kirigami.Icon {
            visible: chip.diverted
            source: "data-warning"
            color: Kirigami.Theme.neutralTextColor
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            Layout.rightMargin: Kirigami.Units.smallSpacing
            Accessible.ignored: true
        }
        Kirigami.Icon {
            source: chip.appIcon || "application-x-executable"
            fallback: "application-x-executable"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            Layout.rightMargin: Kirigami.Units.smallSpacing
            Accessible.ignored: true
        }
        QQC2.Label {
            id: nameLabel
            text: chip.appName
            elide: Text.ElideRight
            font: Kirigami.Theme.smallFont
            Layout.fillWidth: true
        }
        QQC2.ToolButton {
            icon.name: "overflow-menu"
            icon.width: Kirigami.Units.iconSizes.small
            icon.height: Kirigami.Units.iconSizes.small
            implicitWidth: implicitHeight
            implicitHeight: chip.implicitHeight
            display: QQC2.AbstractButton.IconOnly
            text: i18nc("@action:button accessible", "%1 options", chip.appName)
            onClicked: menu.popup()

            QQC2.Menu {
                id: menu
                QQC2.MenuItem {
                    visible: chip.diverted
                    height: visible ? implicitHeight : 0
                    text: i18nc("@action:inmenu", "Move Back to This Bus")
                    icon.name: "go-previous"
                    onTriggered: Mixer.reclaimStreams(chip.divertedIds)
                }
                QQC2.Menu {
                    id: moveMenu
                    title: i18nc("@action:inmenu", "Move to")
                    Instantiator {
                        model: Mixer.buses
                        delegate: QQC2.MenuItem {
                            required property string busId
                            required property string name
                            required property bool isInput
                            text: name
                            visible: !isInput && busId !== chip.busId
                            height: visible ? implicitHeight : 0
                            onTriggered: Mixer.assignApp(chip.appKey, busId)
                        }
                        onObjectAdded: (index, object) => moveMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => moveMenu.removeItem(object)
                    }
                }
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Unassign")
                    icon.name: "edit-clear"
                    onTriggered: Mixer.unassignApp(chip.appKey)
                }
            }
        }
    }

    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.MoveAction
    Drag.mimeData: ({ "application/x-rostrum-app": chip.appKey })
    Drag.active: dragHandler.active

    DragHandler {
        id: dragHandler
        target: null
    }
    HoverHandler {
        onHoveredChanged: if (hovered) chip.grabToImage(result => chip.Drag.imageSource = result.url)
    }
}
