import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// 200 px page list that collapses to icons only. Arrow keys move, Enter or Space opens.
QQC2.Pane {
    id: sidebar

    required property var pages
    property int currentIndex: 0
    readonly property bool collapsed: App.sidebarCollapsed
    readonly property bool activeFocusInside: list.activeFocus || collapseButton.activeFocus

    signal pageRequested(string id)

    function focusList() {
        list.currentIndex = sidebar.currentIndex
        list.forceActiveFocus(Qt.TabFocusReason)
    }

    padding: 0
    implicitWidth: collapsed ? Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.largeSpacing * 4 : 200
    Behavior on implicitWidth {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.InOutQuad
        }
    }

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
    }

    Accessible.role: Accessible.Pane
    Accessible.name: i18nc("@title accessible", "Pages")

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            model: sidebar.pages
            clip: true
            keyNavigationEnabled: true
            activeFocusOnTab: true
            highlightFollowsCurrentItem: false
            currentIndex: sidebar.currentIndex
            Accessible.role: Accessible.List
            Accessible.name: i18nc("@title accessible", "Pages")

            Keys.onReturnPressed: sidebar.pageRequested(sidebar.pages[currentIndex].id)
            Keys.onEnterPressed: sidebar.pageRequested(sidebar.pages[currentIndex].id)
            Keys.onSpacePressed: sidebar.pageRequested(sidebar.pages[currentIndex].id)

            delegate: QQC2.ItemDelegate {
                id: entry
                required property var modelData
                required property int index

                width: ListView.view.width
                text: modelData.label
                icon.name: modelData.icon
                display: sidebar.collapsed ? QQC2.AbstractButton.IconOnly : QQC2.AbstractButton.TextBesideIcon
                highlighted: index === sidebar.currentIndex
                focusPolicy: Qt.NoFocus
                leftPadding: Kirigami.Units.largeSpacing * 2
                topPadding: Kirigami.Units.mediumSpacing
                bottomPadding: Kirigami.Units.mediumSpacing

                // Keyboard cursor, separate from the open page.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: list.activeFocus && list.currentIndex === entry.index
                    color: "transparent"
                    border.color: Kirigami.Theme.focusColor
                    border.width: 2
                    radius: Kirigami.Units.cornerRadius
                }

                Accessible.name: modelData.label
                Accessible.role: Accessible.PageTab
                Accessible.selected: highlighted

                QQC2.ToolTip.text: modelData.label
                QQC2.ToolTip.visible: sidebar.collapsed && hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

                onClicked: sidebar.pageRequested(modelData.id)
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        QQC2.ToolButton {
            id: collapseButton
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            icon.name: sidebar.collapsed ? "sidebar-expand-left" : "sidebar-collapse-left"
            text: sidebar.collapsed ? i18nc("@action:button", "Expand Sidebar") : i18nc("@action:button", "Collapse Sidebar")
            display: sidebar.collapsed ? QQC2.AbstractButton.IconOnly : QQC2.AbstractButton.TextBesideIcon
            onClicked: App.sidebarCollapsed = !App.sidebarCollapsed
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: sidebar.collapsed && hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
}
