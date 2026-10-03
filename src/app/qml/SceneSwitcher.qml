import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// "Scene: Live ⌄" with a dot while faders have moved since the last save.
QQC2.ToolButton {
    id: button

    signal switchRequested(string name)
    signal manageRequested()

    down: menu.visible
    onPressed: menu.popup(button, 0, button.height)
    Keys.onReturnPressed: menu.popup(button, 0, button.height)
    Keys.onSpacePressed: menu.popup(button, 0, button.height)

    Accessible.role: Accessible.ButtonMenu
    Accessible.name: i18nc("@action:button", "Scene")
    Accessible.description: App.sceneDirty ? i18n("%1, unsaved fader moves", App.currentScene) : App.currentScene

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label {
            text: i18nc("@label followed by the scene name", "Scene:")
            opacity: 0.7
        }
        QQC2.Label {
            text: App.currentScene
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.maximumWidth: Kirigami.Units.gridUnit * 12
        }
        Rectangle {
            visible: App.sceneDirty
            implicitWidth: Kirigami.Units.smallSpacing * 2
            implicitHeight: implicitWidth
            radius: width / 2
            color: Kirigami.Theme.highlightColor
            QQC2.ToolTip.text: i18n("Faders moved since the last save")
            QQC2.ToolTip.visible: dotHover.hovered
            HoverHandler {
                id: dotHover
            }
        }
        Kirigami.Icon {
            source: "arrow-down"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
        }
    }

    QQC2.Menu {
        id: menu

        Instantiator {
            model: App.sceneNames
            delegate: QQC2.MenuItem {
                required property string modelData
                text: modelData
                checkable: true
                checked: modelData === App.currentScene
                onTriggered: button.switchRequested(modelData)
            }
            onObjectAdded: (index, object) => menu.insertItem(index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }
        QQC2.MenuSeparator {}
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Save Scene")
            icon.name: "document-save"
            onTriggered: App.saveScene()
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Manage Scenes…")
            icon.name: "view-media-playlist"
            onTriggered: button.manageRequested()
        }
    }
}
