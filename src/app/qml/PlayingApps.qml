import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kitemmodels as KItemModels

// Presentation only: both groups use the original rows and app controls.
QQC2.ScrollView {
    id: view
    required property var sourceModel
    required property Component appDelegate
    property string query: ""
    property string busFilter: ""
    property bool toolsExpanded: false
    readonly property string normalizedQuery: query.trim().toLowerCase()
    readonly property bool toolsVisible: toolsExpanded
    readonly property int playingCount: playingShown.count
    readonly property int toolsCount: toolsShown.count
    contentWidth: availableWidth

    function isTool(row) {
        return row.tool === true && !row.busId
    }
    function matches(row) {
        return (busFilter === "" || row.busId === busFilter) &&
            (normalizedQuery === "" || [row.name, row.binary, row.busName].some(
                f => (f || "").toLowerCase().includes(normalizedQuery)))
    }
    onNormalizedQueryChanged: {
        playingShown.invalidateFilter()
        toolsShown.invalidateFilter()
    }
    onBusFilterChanged: {
        playingShown.invalidateFilter()
        toolsShown.invalidateFilter()
    }

    KItemModels.KSortFilterProxyModel {
        id: playingShown
        sourceModel: view.sourceModel
        filterRole: Qt.UserRole + 1
        filterRowCallback: (sourceRow, sourceParent) => {
            const row = sourceModel.data(sourceModel.index(sourceRow, 0, sourceParent), Qt.UserRole + 1)
            return (!view.isTool(row) || view.normalizedQuery !== "") && view.matches(row)
        }
    }
    KItemModels.KSortFilterProxyModel {
        id: toolsShown
        sourceModel: view.sourceModel
        filterRole: Qt.UserRole + 1
        filterRowCallback: (sourceRow, sourceParent) => {
            const row = sourceModel.data(sourceModel.index(sourceRow, 0, sourceParent), Qt.UserRole + 1)
            return view.normalizedQuery === "" && view.isTool(row) && view.matches(row)
        }
    }

    ListView {
        id: playing
        objectName: "playingAppsList"
        width: view.availableWidth
        height: view.availableHeight
        model: playingShown
        spacing: Kirigami.Units.smallSpacing
        clip: true
        delegate: view.appDelegate
        header: QQC2.Label {
            width: playing.width
            height: playing.count === 0 ? implicitHeight + Kirigami.Units.largeSpacing * 2 : 0
            visible: playing.count === 0
            wrapMode: Text.WordWrap
            text: view.normalizedQuery !== "" || view.busFilter !== ""
                ? i18nc("@info", "No playing apps match")
                : view.toolsCount > 0
                    ? i18nc("@info", "No other apps are playing")
                    : i18nc("@info", "No apps are playing. Launch Discord or a game to see it here.")
            Accessible.name: text
        }
        footer: ColumnLayout {
            width: playing.width
            height: visible ? implicitHeight : 0
            visible: view.toolsCount > 0
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing
                Layout.bottomMargin: Kirigami.Units.smallSpacing
            }
            QQC2.ToolButton {
                objectName: "audioToolsToggle"
                Layout.maximumWidth: view.availableWidth
                text: i18nc("@title", "Audio tools and services (%1)", view.toolsCount)
                icon.name: view.toolsVisible ? "go-down" : "go-next"
                checkable: true
                checked: view.toolsVisible
                onClicked: view.toolsExpanded = !view.toolsExpanded
                Accessible.name: view.toolsVisible
                    ? i18nc("@action:button accessible", "Hide audio tools and services (%1)", view.toolsCount)
                    : i18nc("@action:button accessible", "Show audio tools and services (%1)", view.toolsCount)
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: view.toolsVisible
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: i18nc("@info", "These streams keep their existing audio routing. Rostrum does not assign them automatically.")
                    Accessible.name: text
                }
                Repeater {
                    model: toolsShown
                    delegate: view.appDelegate
                }
            }
        }
    }
}
