import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kitemmodels as KItemModels
import Rostrum

// Running apps on the left, saved rules on the right. One search box filters both. Opened from
// a strip's "+N", both lists show only that bus until the filter chip is cleared.
QQC2.Pane {
    id: page

    padding: Kirigami.Units.largeSpacing
    focusPolicy: Qt.NoFocus

    readonly property string query: search.text.trim().toLowerCase()
    readonly property string busFilter: App.appsFilter
    readonly property string busFilterName: {
        const b = Apps.buses.find(b => b.id === busFilter)
        return b ? b.name : ""
    }

    function matches(fields, busId) {
        if (busFilter !== "" && busId !== busFilter) {
            return false
        }
        return query === "" || fields.some(f => (f || "").toLowerCase().includes(query))
    }
    onQueryChanged: {
        runningShown.invalidateFilter()
        rulesShown.invalidateFilter()
    }
    onBusFilterChanged: {
        runningShown.invalidateFilter()
        rulesShown.invalidateFilter()
    }

    // Role Qt.UserRole + 1 is the whole row map (RowsModel).
    KItemModels.KSortFilterProxyModel {
        id: runningShown
        sourceModel: Apps.runningModel
        filterRowCallback: (sourceRow, sourceParent) => {
            const a = sourceModel.data(sourceModel.index(sourceRow, 0, sourceParent), Qt.UserRole + 1)
            return page.matches([a.name, a.binary, a.busName], a.busId)
        }
    }
    KItemModels.KSortFilterProxyModel {
        id: rulesShown
        sourceModel: Apps.rulesModel
        filterRowCallback: (sourceRow, sourceParent) => {
            const r = sourceModel.data(sourceModel.index(sourceRow, 0, sourceParent), Qt.UserRole + 1)
            return page.matches([r.match, r.label, r.busName], r.busId)
        }
    }

    readonly property bool wantMeters: page.visible && page.Window.window !== null && page.Window.window.visible
    onWantMetersChanged: Apps.metersActive = wantMeters
    onVisibleChanged: if (!visible) App.appsFilter = ""

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            Kirigami.SearchField {
                id: search
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                placeholderText: i18nc("@info:placeholder", "Search apps and rules…")
            }
            QQC2.Button {
                visible: page.busFilter !== ""
                text: i18nc("@action:button %1 is a bus", "Only %1", page.busFilterName)
                icon.name: "edit-clear"
                onClicked: App.appsFilter = ""
                Accessible.name: i18nc("@action:button accessible", "Showing only %1. Show all buses", page.busFilterName)
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Kirigami.InlineMessage {
            id: unnamedBanner
            Layout.fillWidth: true
            visible: Apps.unnamed.key !== undefined
            type: Kirigami.MessageType.Information
            text: i18n("An app is playing audio without a name. Its program is “%1”. Name it to give it a rule.",
                       Apps.unnamed.binary || i18nc("unknown program", "unknown"))
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Name It…")
                    icon.name: "document-edit"
                    onTriggered: nameDialog.openFor(Apps.unnamed)
                }
            ]
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: page.width > Kirigami.Units.gridUnit * 44 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing * 2
            rowSpacing: Kirigami.Units.largeSpacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 3
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Heading {
                    level: 2
                    text: i18nc("@title", "Playing now")
                }
                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ListView {
                        id: runningList
                        model: runningShown
                        spacing: Kirigami.Units.smallSpacing
                        clip: true
                        delegate: AppRow {
                            required property var row
                            app: row
                            width: ListView.view.width
                        }
                        Kirigami.PlaceholderMessage {
                            anchors.centerIn: parent
                            width: parent.width - Kirigami.Units.gridUnit * 2
                            visible: runningList.count === 0
                            icon.name: "applications-multimedia"
                            text: Apps.running.length === 0 ? i18n("No apps are playing")
                                                            : i18n("No playing apps match")
                            explanation: Apps.running.length === 0
                                         ? i18n("Launch Discord or a game. It will show up here while it is making audio.")
                                         : ""
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Heading {
                    level: 2
                    text: i18nc("@title", "Saved rules")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: i18nc("@info %1 is a scene", "Rules in “%1”. New launches of these apps go straight to their bus.",
                                App.currentScene)
                }
                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ListView {
                        id: rulesList
                        model: rulesShown
                        spacing: Kirigami.Units.smallSpacing
                        clip: true
                        delegate: RuleRow {
                            required property var row
                            rule: row
                            width: ListView.view.width
                        }
                        Kirigami.PlaceholderMessage {
                            anchors.centerIn: parent
                            width: parent.width - Kirigami.Units.gridUnit * 2
                            visible: rulesList.count === 0
                            icon.name: "view-filter"
                            text: Apps.rules.length === 0 ? i18n("No rules yet") : i18n("No rules match")
                            explanation: Apps.rules.length === 0
                                         ? i18n("Assign a playing app with Always on and it gets a rule.")
                                         : ""
                        }
                    }
                }
            }
        }
    }

    Kirigami.Dialog {
        id: nameDialog
        property string appKey
        property string binary

        function openFor(app) {
            appKey = app.key
            binary = app.binary || ""
            nameField.text = ""
            busBox.currentIndex = Math.max(0, Apps.buses.findIndex(b => b.id === "desktop"))
            open()
            nameField.forceActiveFocus()
        }

        title: i18nc("@title:dialog", "Name This App")
        padding: Kirigami.Units.largeSpacing
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Save Rule")
                icon.name: "dialog-ok"
                enabled: nameField.text.trim().length > 0 && busBox.currentIndex >= 0
                onTriggered: {
                    Apps.nameApp(nameDialog.appKey, nameField.text, Apps.buses[busBox.currentIndex].id)
                    nameDialog.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: nameDialog.close()
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing
            QQC2.Label {
                Layout.fillWidth: true
                Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                wrapMode: Text.WordWrap
                text: i18n("Program: %1", nameDialog.binary || i18nc("unknown program", "unknown"))
                opacity: 0.7
            }
            QQC2.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: i18nc("@info:placeholder", "Name, for example “Game client”")
                Accessible.name: i18nc("@label accessible", "App name")
            }
            QQC2.ComboBox {
                id: busBox
                Layout.fillWidth: true
                model: Apps.buses.map(b => b.name)
                Accessible.name: i18nc("@label accessible", "Send to bus")
            }
        }
    }
}
