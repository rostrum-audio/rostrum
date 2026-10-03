import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtCore
import org.kde.kirigami as Kirigami
import Rostrum

// Scene management. Selecting a row never loads it, so browsing cannot wreck a live mix; Load
// does. The header switcher is the live control.
QQC2.Pane {
    id: page

    padding: Kirigami.Units.largeSpacing
    focusPolicy: Qt.NoFocus

    property string selectedName: App.currentScene
    readonly property var selected: Scenes.rows.find(r => r.name === selectedName) ?? null
    readonly property int selectedIndex: Scenes.rows.findIndex(r => r.name === selectedName)

    Component {
        id: presetActionComponent
        Kirigami.Action {
            required property var preset
            text: i18nc("@action:inmenu scene preset", "%1…", preset.name)
            icon.name: preset.icon
            tooltip: preset.description
            onTriggered: presetDialog.openPreset(preset)
        }
    }
    Component.onCompleted: {
        const items = Scenes.presets.map(p => presetActionComponent.createObject(page, { preset: p }))
        newAction.children = Array.from(newAction.children).concat(items)
    }

    Connections {
        target: Scenes
        function onChanged() {
            if (!Scenes.rows.some(r => r.name === page.selectedName)) {
                page.selectedName = App.currentScene
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.ActionToolBar {
            Layout.fillWidth: true
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Load")
                    icon.name: "media-playback-start"
                    enabled: page.selected !== null && !page.selected.isCurrent
                    onTriggered: applicationWindow().requestSceneSwitch(page.selectedName)
                },
                Kirigami.Action {
                    id: newAction
                    text: i18nc("@action:button", "New")
                    icon.name: "list-add"
                    Kirigami.Action {
                        text: i18nc("@action:inmenu", "Empty Scene…")
                        icon.name: "document-new"
                        onTriggered: {
                            newDialog.except = ""
                            newDialog.openWith(Scenes.uniqueName(i18nc("default name for a new scene", "New scene")))
                        }
                    }
                    Kirigami.Action {
                        separator: true
                    }
                    Kirigami.Action {
                        text: i18nc("@title:menu section", "From a Preset")
                        enabled: false
                    }
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Duplicate")
                    icon.name: "edit-copy"
                    enabled: page.selected !== null
                    onTriggered: {
                        const name = Scenes.duplicate(page.selectedName)
                        if (name) {
                            page.selectedName = name
                        }
                    }
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Rename")
                    icon.name: "edit-rename"
                    enabled: page.selected !== null
                    onTriggered: {
                        renameDialog.except = page.selectedName
                        renameDialog.openWith(page.selectedName)
                    }
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Move Up")
                    icon.name: "go-up"
                    tooltip: i18n("Scenes keep this order in the header, the tray and the Load scene hotkeys. Alt+Up also moves the selected scene.")
                    displayHint: Kirigami.DisplayHint.IconOnly
                    enabled: page.selectedIndex > 0
                    onTriggered: Scenes.moveBy(page.selectedName, -1)
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Move Down")
                    icon.name: "go-down"
                    tooltip: i18n("Scenes keep this order in the header, the tray and the Load scene hotkeys. Alt+Down also moves the selected scene.")
                    displayHint: Kirigami.DisplayHint.IconOnly
                    enabled: page.selectedIndex >= 0 && page.selectedIndex < Scenes.rows.length - 1
                    onTriggered: Scenes.moveBy(page.selectedName, 1)
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Set as Default")
                    icon.name: "favorite"
                    enabled: page.selected !== null && !page.selected.isDefault
                    onTriggered: Scenes.setDefault(page.selectedName)
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Delete")
                    icon.name: "edit-delete"
                    enabled: page.selected !== null && Scenes.rows.length > 1
                    onTriggered: deleteDialog.open()
                },
                Kirigami.Action {
                    separator: true
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Export…")
                    icon.name: "document-export"
                    displayHint: Kirigami.DisplayHint.AlwaysHide
                    onTriggered: exportDialog.open()
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Import…")
                    icon.name: "document-import"
                    displayHint: Kirigami.DisplayHint.AlwaysHide
                    onTriggered: importDialog.open()
                }
            ]
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                id: list
                model: Scenes.rows
                clip: true
                keyNavigationEnabled: true
                currentIndex: Scenes.rows.findIndex(r => r.name === page.selectedName)
                Accessible.name: i18nc("@title", "Scenes")

                delegate: QQC2.ItemDelegate {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property color textColor: highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                    width: ListView.view.width
                    highlighted: ListView.isCurrentItem
                    onClicked: {
                        page.selectedName = modelData.name
                        list.forceActiveFocus()
                    }
                    onDoubleClicked: applicationWindow().requestSceneSwitch(modelData.name)
                    Accessible.name: modelData.name
                    Accessible.description: [modelData.isCurrent ? i18n("Live now") : "",
                                             modelData.isDefault ? i18n("Default on launch") : "",
                                             row.slotText,
                                             modelData.summary].filter(s => s).join(", ")
                    readonly property string slotText: modelData.slot <= 0 ? ""
                        : modelData.shortcut ? i18nc("@info:tooltip scene hotkey slot", "Load scene %1: %2", modelData.slot, modelData.shortcut)
                        : i18nc("@info:tooltip scene hotkey slot", "Load scene %1 (no shortcut set)", modelData.slot)

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: row.modelData.isDefault ? "rating" : ""
                            color: row.textColor
                            isMask: true
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: implicitWidth
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            RowLayout {
                                Rectangle {
                                    opacity: row.modelData.slot > 0 ? 1 : 0
                                    implicitWidth: Math.max(implicitHeight, slotLabel.implicitWidth + Kirigami.Units.smallSpacing * 2)
                                    implicitHeight: slotLabel.implicitHeight + 2
                                    radius: height / 2
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Qt.alpha(row.textColor, 0.5)
                                    QQC2.Label {
                                        id: slotLabel
                                        anchors.centerIn: parent
                                        text: row.modelData.slot > 0 ? row.modelData.slot : "0"
                                        color: row.textColor
                                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                                    }
                                    HoverHandler {
                                        id: slotHover
                                    }
                                    QQC2.ToolTip.text: row.slotText
                                    QQC2.ToolTip.visible: slotHover.hovered && row.modelData.slot > 0
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                }
                                QQC2.Label {
                                    text: row.modelData.name
                                    color: row.textColor
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                QQC2.Label {
                                    visible: row.modelData.isCurrent
                                    text: App.sceneDirty && !Preferences.autoSaveScenes
                                          ? i18nc("@info scene state", "Live, unsaved fader moves")
                                          : i18nc("@info scene state", "Live")
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                    font.weight: Font.DemiBold
                                    color: row.highlighted ? row.textColor : Kirigami.Theme.positiveTextColor
                                }
                                QQC2.Label {
                                    visible: row.modelData.isDefault
                                    text: i18nc("@info scene state", "Default")
                                    color: row.textColor
                                    font: Kirigami.Theme.smallFont
                                    opacity: 0.7
                                }
                            }
                            QQC2.Label {
                                text: row.modelData.summary
                                color: row.textColor
                                font: Kirigami.Theme.smallFont
                                opacity: 0.7
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                }

                Keys.onReturnPressed: applicationWindow().requestSceneSwitch(page.selectedName)
                Keys.onPressed: event => {
                    if ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                        Scenes.moveBy(page.selectedName, event.key === Qt.Key_Up ? -1 : 1)
                        event.accepted = true
                    }
                }
                onCurrentIndexChanged: if (currentIndex >= 0 && currentIndex < Scenes.rows.length) {
                    page.selectedName = Scenes.rows[currentIndex].name
                }
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            font: Kirigami.Theme.smallFont
            text: Preferences.autoSaveScenes
                  ? i18n("Selecting a scene does not change your mix. Press Load, or switch from the header. Changes to the live scene save automatically, and the default scene loads when Rostrum starts.")
                  : i18n("Selecting a scene does not change your mix. Press Load, or switch from the header. Save keeps fader moves in the live scene, and the default scene loads when Rostrum starts.")
        }
    }

    SceneNameDialog {
        id: newDialog
        title: i18nc("@title:dialog", "New Scene")
        actionText: i18nc("@action:button", "Create")
        onNameChosen: name => {
            const created = Scenes.createScene(name)
            if (created) {
                page.selectedName = created
            }
        }
    }

    SceneNameDialog {
        id: presetDialog
        property string presetId
        title: i18nc("@title:dialog", "New Scene from Preset")
        actionText: i18nc("@action:button", "Create")
        note: i18n("Uses your current buses and app rules. Only levels, mutes and destinations come from the preset.")
        function openPreset(preset) {
            presetId = preset.id
            heading = preset.name
            description = preset.description
            iconName = preset.icon
            except = ""
            openWith(Scenes.uniqueName(preset.name))
        }
        onNameChosen: name => {
            const created = Scenes.createFromPreset(presetDialog.presetId, name)
            if (created) {
                page.selectedName = created
            }
        }
    }

    SceneNameDialog {
        id: renameDialog
        title: i18nc("@title:dialog", "Rename Scene")
        actionText: i18nc("@action:button", "Rename")
        onNameChosen: name => {
            const renamed = Scenes.rename(renameDialog.except, name)
            if (renamed) {
                page.selectedName = renamed
            }
        }
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        title: i18nc("@title:dialog", "Delete “%1”?", page.selectedName)
        subtitle: page.selected && page.selected.isCurrent
                  ? i18n("This is the live scene. Rostrum switches to the default scene after deleting it.")
                  : i18n("The scene file is removed. Export first if you want a backup.")
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Delete")
                icon.name: "edit-delete"
                onTriggered: {
                    Scenes.remove(page.selectedName)
                    deleteDialog.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: deleteDialog.close()
            }
        ]
    }

    Dialogs.FileDialog {
        id: exportDialog
        title: i18nc("@title:window", "Export Scenes")
        fileMode: Dialogs.FileDialog.SaveFile
        defaultSuffix: "toml"
        nameFilters: [i18nc("file filter", "Rostrum scenes (*.toml)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: Scenes.exportTo(selectedFile)
    }

    Dialogs.FileDialog {
        id: importDialog
        title: i18nc("@title:window", "Import Scenes")
        fileMode: Dialogs.FileDialog.OpenFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        nameFilters: [i18nc("file filter", "Rostrum scenes (*.toml)"), i18nc("file filter", "All files (*)")]
        onAccepted: Scenes.importFrom(selectedFile)
    }
}
