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

        RowLayout {
            spacing: Kirigami.Units.smallSpacing * 1.5
            Accessible.ignored: true

            Image {
                source: "qrc:/icons/dev.getrostrum.Rostrum-tray.svg"
                sourceSize.width: Kirigami.Units.iconSizes.smallMedium * 2
                sourceSize.height: Kirigami.Units.iconSizes.smallMedium * 2
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                smooth: true
                mipmap: true
            }
            QQC2.Label {
                text: i18nc("@title app name", "Rostrum")
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.25
                font.weight: Font.Bold
                font.letterSpacing: -0.3
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            Layout.leftMargin: Kirigami.Units.smallSpacing
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
