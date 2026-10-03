import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Rostrum

Kirigami.ApplicationWindow {
    id: root

    title: App.currentScene.length > 0 ? i18nc("@title:window scene name", "%1 — Rostrum", App.currentScene)
                                       : i18nc("@title:window", "Rostrum")
    minimumWidth: 960
    minimumHeight: 600

    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.None
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn
    pageStack.initialPage: Shell {
        id: shell
    }

    header: HeaderBar {
        id: headerBar
        window: root
    }
    footer: StatusBar {}

    function showPage(page) {
        if (App.pipewireState !== "missing") {
            App.lastPage = page
        }
    }

    // Shared by the header switcher, the menu and later the hotkeys.
    function requestSceneSwitch(name) {
        if (name === App.currentScene) {
            return
        }
        if (App.confirmSceneSwitch && App.sceneDirty) {
            switchDialog.target = name
            switchDialog.open()
            return
        }
        App.switchScene(name)
    }

    Component.onCompleted: {
        width = App.windowWidth
        height = App.windowHeight
    }
    onWidthChanged: if (visibility === Window.Windowed) App.windowWidth = width
    onHeightChanged: if (visibility === Window.Windowed) App.windowHeight = height
    onClosing: App.saveSettingsNow()

    Connections {
        target: App
        function onToast(message) {
            root.showPassiveNotification(message)
        }
        function onRaiseRequested() {
            root.show()
            root.raise()
            root.requestActivate()
        }
    }

    // F6 moves focus between the header, the sidebar and the page, like other KDE apps.
    Shortcut {
        sequences: ["F6"]
        onActivated: {
            if (headerBar.activeFocusInside) {
                shell.focusSidebar()
            } else if (shell.sidebarHasFocus) {
                shell.focusPage()
            } else {
                headerBar.focusFirst()
            }
        }
    }
    Shortcut {
        sequences: ["Shift+F6"]
        onActivated: {
            if (headerBar.activeFocusInside) {
                shell.focusPage()
            } else if (shell.sidebarHasFocus) {
                headerBar.focusFirst()
            } else {
                shell.focusSidebar()
            }
        }
    }
    Shortcut {
        sequences: [StandardKey.Save]
        enabled: App.connected
        onActivated: App.saveScene()
    }
    Shortcut {
        sequences: [StandardKey.Quit]
        onActivated: App.quit()
    }
    Shortcut {
        sequences: ["Ctrl+M"]
        onActivated: App.toggleMicMute()
    }

    Kirigami.PromptDialog {
        id: switchDialog
        property string target
        title: i18nc("@title:dialog", "Discard fader moves?")
        subtitle: i18n("Faders in “%1” have moved since the last save. Switching to “%2” discards those moves.",
                       App.currentScene, target)
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Save and Switch")
                icon.name: "document-save"
                onTriggered: {
                    if (App.saveScene()) {
                        App.switchScene(switchDialog.target)
                    }
                    switchDialog.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Switch")
                onTriggered: {
                    App.switchScene(switchDialog.target)
                    switchDialog.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: switchDialog.close()
            }
        ]
    }
}
