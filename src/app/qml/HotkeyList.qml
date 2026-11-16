import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kquickcontrols as KQuickControls

// The Hotkeys tab: how shortcuts work, a search field, then one collapsible card per group.
// Searching only filters what is shown; it opens every group that has a match and leaves the
// groups as they were once the field is cleared.
ColumnLayout {
    id: list

    // Preferences in the app: hotkeys (rows of {id, group, label, description, shortcut,
    // defaultShortcut, problem}), hotkeyConflict(), hotkeySequenceConflict(), setHotkeySequence(),
    // replaceHotkeySequence(), setHotkey(), resetHotkey().
    required property var preferences
    // "kglobalaccel", "portal" or anything else for shortcuts that only work in the window.
    property string backend
    property real maximumWidth: Kirigami.Units.gridUnit * 52

    readonly property string query: search.text.trim().toLowerCase()
    readonly property var groups: [
        { group: "mic", title: i18nc("@title:group hotkeys", "Mic"), open: true },
        { group: "stream", title: i18nc("@title:group hotkeys", "Stream and Headphones"), open: true },
        { group: "scenes", title: i18nc("@title:group hotkeys", "Scenes"), open: false },
        { group: "buses", title: i18nc("@title:group hotkeys", "Buses"), open: false }
    ]
    readonly property int matches: preferences.hotkeys.filter(row => list.matchesQuery(row)).length

    function matchesQuery(row) {
        if (query === "")
            return true
        return [row.label, row.description, row.shortcut].some(text => String(text ?? "").toLowerCase().includes(query))
    }

    spacing: 0

    FormCard.FormCard {
        maximumWidth: list.maximumWidth
        Layout.topMargin: Kirigami.Units.largeSpacing
        FormCard.FormTextDelegate {
            text: i18n("Click a shortcut, then press the keys. Backspace or the clear button removes it.")
            description: list.backend === "kglobalaccel"
                         ? i18n("They work in any app and also appear in System Settings → Shortcuts.")
                         : list.backend === "portal"
                           ? i18n("They work in any app once the desktop allows them. It may ask you to confirm.")
                           : ""
            textItem.wrapMode: Text.WordWrap
        }
        FormCard.FormTextDelegate {
            text: i18n("Push to talk and push to mute act while the keys are held. Where the desktop does not report the keys going up, and in the window, each press turns them on or off instead.")
            textItem.wrapMode: Text.WordWrap
            textItem.font: Kirigami.Theme.smallFont
            textItem.opacity: 0.7
        }
        FormCard.AbstractFormDelegate {
            Layout.fillWidth: true
            background: null
            focusPolicy: Qt.NoFocus
            contentItem: Kirigami.SearchField {
                id: search
                objectName: "hotkeySearch"
                placeholderText: i18nc("@info:placeholder", "Search hotkeys…")
                Accessible.name: i18nc("@label accessible", "Search hotkeys")
            }
        }
    }

    FormCard.FormCard {
        maximumWidth: list.maximumWidth
        Layout.topMargin: Kirigami.Units.largeSpacing
        visible: list.matches === 0
        FormCard.FormTextDelegate {
            objectName: "hotkeyNoMatches"
            text: i18nc("@info", "No hotkeys match “%1”.", search.text.trim())
            textItem.wrapMode: Text.WordWrap
        }
    }

    Repeater {
        model: list.groups
        delegate: FormCard.FormCard {
            id: hotkeyGroup
            required property var modelData
            readonly property var all: list.preferences.hotkeys.filter(row => row.group === modelData.group)
            readonly property var rows: all.filter(row => list.matchesQuery(row))
            // A shortcut that clashes or that the desktop refused must not hide in a closed group.
            readonly property bool needsReview: all.some(row => row.problem !== ""
                                                          || list.preferences.hotkeyConflict(row.id, row.shortcut) !== "")
            property bool open: modelData.open
            readonly property bool expanded: open || list.query !== ""
            onNeedsReviewChanged: if (needsReview) open = true
            Component.onCompleted: if (needsReview) open = true

            objectName: "hotkeyGroup-" + modelData.group
            maximumWidth: list.maximumWidth
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: rows.length > 0

            FormCard.AbstractFormDelegate {
                objectName: "hotkeyGroupToggle-" + hotkeyGroup.modelData.group
                Layout.fillWidth: true
                text: hotkeyGroup.modelData.title
                enabled: list.query === ""
                onClicked: hotkeyGroup.open = !hotkeyGroup.open
                Accessible.name: text
                Accessible.description: hotkeyGroup.expanded ? i18n("Expanded") : i18n("Collapsed")
                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        source: hotkeyGroup.expanded ? "arrow-down" : "arrow-right"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: implicitWidth
                        Accessible.ignored: true
                    }
                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 4
                        text: hotkeyGroup.modelData.title
                        elide: Text.ElideRight
                    }
                }
            }
            Repeater {
                model: hotkeyGroup.expanded ? hotkeyGroup.rows : []
                delegate: FormCard.AbstractFormDelegate {
                    id: hotkeyRow
                    required property var modelData
                    // A new shortcut another action already has waits here until Replace or Cancel.
                    property bool pending: false
                    readonly property string conflict: pending
                        ? list.preferences.hotkeySequenceConflict(modelData.id, keys.keySequence)
                        : list.preferences.hotkeyConflict(modelData.id, modelData.shortcut)
                    objectName: "hotkeyRow-" + modelData.id
                    Layout.fillWidth: true
                    background: null
                    focusPolicy: Qt.NoFocus
                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            QQC2.Label {
                                text: hotkeyRow.modelData.label
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            QQC2.Label {
                                visible: text !== ""
                                text: hotkeyRow.modelData.description
                                font: Kirigami.Theme.smallFont
                                opacity: 0.7
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            QQC2.Label {
                                visible: hotkeyRow.conflict !== ""
                                text: i18nc("@info", "Also used by “%1”", hotkeyRow.conflict)
                                color: Kirigami.Theme.neutralTextColor
                                font: Kirigami.Theme.smallFont
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            // Replace takes the shortcut away from the other action; Cancel keeps things as
                            // they were. A clash that is already saved has nothing to go back to, so it
                            // only offers Replace.
                            RowLayout {
                                visible: hotkeyRow.conflict !== ""
                                Layout.topMargin: Kirigami.Units.smallSpacing
                                spacing: Kirigami.Units.smallSpacing
                                QQC2.Button {
                                    objectName: "hotkeyReplace-" + hotkeyRow.modelData.id
                                    text: i18nc("@action:button take the shortcut from the other action", "Replace")
                                    Accessible.name: i18nc("@action:button accessible; %1 is the action that loses the shortcut, %2 the one that gets it",
                                                           "Replace: remove the shortcut from %1 and use it for %2",
                                                           hotkeyRow.conflict, hotkeyRow.modelData.label)
                                    onClicked: {
                                        hotkeyRow.pending = false
                                        list.preferences.replaceHotkeySequence(hotkeyRow.modelData.id, keys.keySequence)
                                    }
                                }
                                QQC2.Button {
                                    objectName: "hotkeyCancel-" + hotkeyRow.modelData.id
                                    visible: hotkeyRow.pending
                                    text: i18nc("@action:button keep the old shortcut", "Cancel")
                                    Accessible.name: i18nc("@action:button accessible", "Cancel: keep the old shortcut for %1",
                                                           hotkeyRow.modelData.label)
                                    onClicked: {
                                        hotkeyRow.pending = false
                                        keys.keySequence = hotkeyRow.modelData.shortcut
                                    }
                                }
                            }
                            QQC2.Label {
                                visible: hotkeyRow.conflict === "" && hotkeyRow.modelData.problem !== ""
                                text: hotkeyRow.modelData.problem
                                color: Kirigami.Theme.neutralTextColor
                                font: Kirigami.Theme.smallFont
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                        KQuickControls.KeySequenceItem {
                            id: keys
                            objectName: "hotkeyKeys-" + hotkeyRow.modelData.id
                            keySequence: hotkeyRow.modelData.shortcut
                            modifierlessAllowed: false
                            multiKeyShortcutsAllowed: false
                            onKeySequenceModified: {
                                if (list.preferences.hotkeySequenceConflict(hotkeyRow.modelData.id, keySequence) !== "") {
                                    hotkeyRow.pending = true
                                    return
                                }
                                hotkeyRow.pending = false
                                list.preferences.setHotkeySequence(hotkeyRow.modelData.id, keySequence)
                            }
                            Keys.onPressed: event => {
                                if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier) {
                                    hotkeyRow.pending = false
                                    list.preferences.setHotkey(hotkeyRow.modelData.id, "")
                                    event.accepted = true
                                }
                            }
                            Accessible.name: i18nc("@label accessible", "Shortcut for %1", hotkeyRow.modelData.label)
                            Accessible.description: hotkeyRow.modelData.description
                        }
                        QQC2.ToolButton {
                            icon.name: "edit-undo"
                            text: i18nc("@action:button", "Reset to default")
                            display: QQC2.AbstractButton.IconOnly
                            enabled: hotkeyRow.modelData.shortcut !== hotkeyRow.modelData.defaultShortcut
                            onClicked: list.preferences.resetHotkey(hotkeyRow.modelData.id)
                            Accessible.name: i18nc("@action:button accessible", "Reset the shortcut for %1", hotkeyRow.modelData.label)
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.text: text
                        }
                    }
                }
            }
        }
    }
}
