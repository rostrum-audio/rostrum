import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// "SCENE Live ⌄" pill with a dot while faders have moved since the last save.
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
    readonly property bool showDirty: App.sceneDirty && !Preferences.autoSaveScenes

    Accessible.description: button.showDirty ? i18n("%1, unsaved fader moves", App.currentScene) : App.currentScene

    hoverEnabled: true
    leftPadding: Kirigami.Units.largeSpacing * 1.5
    rightPadding: Kirigami.Units.largeSpacing * 1.5
    topPadding: Kirigami.Units.smallSpacing * 1.5
    bottomPadding: Kirigami.Units.smallSpacing * 1.5

    background: Rectangle {
        radius: height / 2
        color: button.down ? Qt.alpha(Kirigami.Theme.textColor, 0.16)
             : button.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.1)
             : Qt.alpha(Kirigami.Theme.textColor, 0.05)
        border.width: button.visualFocus ? 2 : 1
        border.color: button.visualFocus ? Kirigami.Theme.focusColor : Qt.alpha(Kirigami.Theme.textColor, 0.15)
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing * 1.5
        QQC2.Label {
            text: i18nc("@label followed by the scene name", "Scene")
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.8
            opacity: 0.6
        }
        QQC2.Label {
            text: App.currentScene
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.maximumWidth: Kirigami.Units.gridUnit * 12
        }
        Rectangle {
            visible: button.showDirty
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
        Chevron {
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 2
            opacity: 0.75
            up: menu.visible
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
            visible: !Preferences.autoSaveScenes
            height: visible ? implicitHeight : 0
            text: i18nc("@action:inmenu", "Save Scene")
            icon.name: "document-save"
            onTriggered: App.saveScene()
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Save Scene As…")
            icon.name: "document-save-as"
            onTriggered: saveAsDialog.openWith(Scenes.uniqueName(App.currentScene))
        }
        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Manage Scenes…")
            icon.name: "view-media-playlist"
            onTriggered: button.manageRequested()
        }
    }

    SceneNameDialog {
        id: saveAsDialog
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "Save Scene As")
        except: ""
        onNameChosen: name => Scenes.saveAs(name)
    }
}
