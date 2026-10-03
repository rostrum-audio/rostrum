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
    property string busId
    property color busColor: Kirigami.Theme.highlightColor

    readonly property string mimeType: "application/x-rostrum-app"

    implicitHeight: Kirigami.Units.gridUnit * 1.4
    leftPadding: Kirigami.Units.smallSpacing * 2
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true

    Accessible.role: Accessible.StaticText
    Accessible.name: appName

    background: Rectangle {
        radius: height / 2
        color: Qt.alpha(chip.busColor, chip.hovered ? 0.35 : 0.22)
        border.color: Qt.alpha(chip.busColor, 0.6)
    }

    contentItem: RowLayout {
        spacing: 0
        QQC2.Label {
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
        QQC2.ToolButton {
            icon.name: "window-close"
            icon.width: Kirigami.Units.iconSizes.small
            icon.height: Kirigami.Units.iconSizes.small
            implicitWidth: implicitHeight
            implicitHeight: chip.implicitHeight
            display: QQC2.AbstractButton.IconOnly
            text: i18nc("@action:button accessible", "Unassign %1", chip.appName)
            onClicked: Mixer.unassignApp(chip.appKey)
            QQC2.ToolTip.text: i18nc("@info:tooltip", "Unassign")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
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
