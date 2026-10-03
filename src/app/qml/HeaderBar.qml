import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

QQC2.ToolBar {
    id: bar

    required property var window
    readonly property bool activeFocusInside: sceneSwitcher.activeFocus || micButton.activeFocus || menuButton.activeFocus

    function focusFirst() {
        sceneSwitcher.forceActiveFocus(Qt.TabFocusReason)
    }

    position: QQC2.ToolBar.Header
    leftPadding: Kirigami.Units.largeSpacing
    rightPadding: Kirigami.Units.largeSpacing
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing

    RowLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            level: 2
            text: i18nc("@title app name", "Rostrum")
            Accessible.ignored: true
        }

        SceneSwitcher {
            id: sceneSwitcher
            enabled: App.connected
            onSwitchRequested: name => bar.window.requestSceneSwitch(name)
            onManageRequested: bar.window.showPage("scenes")
        }

        Item {
            Layout.fillWidth: true
        }

        MicButton {
            id: micButton
            onDevicesRequested: bar.window.showPage("devices")
        }

        QQC2.ToolButton {
            id: menuButton
            icon.name: "application-menu"
            text: i18nc("@action:button", "Menu")
            display: QQC2.AbstractButton.IconOnly
            down: menu.visible
            onPressed: menu.popup(menuButton, 0, menuButton.height)
            Keys.onReturnPressed: menu.popup(menuButton, 0, menuButton.height)
            Keys.onSpacePressed: menu.popup(menuButton, 0, menuButton.height)
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

            QQC2.Menu {
                id: menu
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Save Scene")
                    icon.name: "document-save"
                    enabled: App.connected
                    onTriggered: App.saveScene()
                }
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Manage Scenes…")
                    icon.name: "view-media-playlist"
                    onTriggered: bar.window.showPage("scenes")
                }
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Settings")
                    icon.name: "settings-configure"
                    onTriggered: bar.window.showPage("settings")
                }
                QQC2.MenuSeparator {}
                QQC2.MenuItem {
                    text: i18nc("@action:inmenu", "Quit")
                    icon.name: "application-exit"
                    onTriggered: App.quit()
                }
            }
        }
    }
}
