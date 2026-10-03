import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// One saved rule: what it matches, where it sends the app, when the app was last seen.
QQC2.Control {
    id: row

    required property var rule
    property bool editing: false

    padding: Kirigami.Units.largeSpacing
    Accessible.role: Accessible.ListItem
    Accessible.name: i18nc("@info accessible: rule match to bus", "%1 to %2", rule.label || rule.match, rule.busName)

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
        radius: Kirigami.Units.cornerRadius
    }

    function startEdit() {
        matchField.text = rule.match
        keyBox.currentIndex = rule.matchKey === "binary" ? 1 : 0
        editing = true
        matchField.forceActiveFocus()
    }
    function commitEdit() {
        if (Apps.editMatch(rule.key, matchField.text, keyBox.currentIndex === 1 ? "binary" : "name")) {
            editing = false
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            visible: !row.editing
            spacing: Kirigami.Units.largeSpacing
            Rectangle {
                implicitWidth: 4
                Layout.fillHeight: true
                color: row.rule.busColor || Kirigami.Theme.disabledTextColor
                radius: 2
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                QQC2.Label {
                    text: i18nc("@info rule: app → bus", "%1 → %2", row.rule.label || row.rule.match, row.rule.busName)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                QQC2.Label {
                    text: {
                        const key = row.rule.matchKey === "binary"
                                  ? i18nc("@info rule key", "binary is “%1”", row.rule.match)
                                  : i18nc("@info rule key", "name is “%1”", row.rule.match)
                        return i18nc("@info rule details: key, last seen", "%1 · %2", key, row.rule.lastSeen)
                    }
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            QQC2.ToolButton {
                icon.name: "document-edit"
                text: i18nc("@action:button", "Edit match")
                display: QQC2.AbstractButton.IconOnly
                onClicked: row.startEdit()
                Accessible.name: i18nc("@action:button accessible", "Edit the match for %1", row.rule.label || row.rule.match)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.text: text
            }
            QQC2.ToolButton {
                icon.name: "edit-delete"
                text: i18nc("@action:button", "Delete rule")
                display: QQC2.AbstractButton.IconOnly
                onClicked: Apps.removeRule(row.rule.key)
                Accessible.name: i18nc("@action:button accessible", "Delete the rule for %1", row.rule.label || row.rule.match)
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.text: text
            }
        }

        RowLayout {
            visible: row.editing
            spacing: Kirigami.Units.smallSpacing
            QQC2.ComboBox {
                id: keyBox
                model: [i18nc("@item:inlistbox match key", "Name"), i18nc("@item:inlistbox match key", "Binary")]
                Accessible.name: i18nc("@label accessible", "Match on")
            }
            QQC2.TextField {
                id: matchField
                Layout.fillWidth: true
                placeholderText: i18nc("@info:placeholder", "Exact app name or binary")
                Accessible.name: i18nc("@label accessible", "Match")
                onAccepted: row.commitEdit()
                Keys.onEscapePressed: row.editing = false
            }
            QQC2.Button {
                text: i18nc("@action:button", "Save")
                icon.name: "dialog-ok"
                enabled: matchField.text.trim().length > 0
                onClicked: row.commitEdit()
            }
            QQC2.Button {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onClicked: row.editing = false
            }
        }
    }
}
