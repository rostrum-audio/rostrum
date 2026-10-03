import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Asks for a scene name and says why a name will not work before the user presses the button.
Kirigami.Dialog {
    id: dialog

    property string except      // the scene being renamed, which may keep its name
    property string actionText: i18nc("@action:button", "Save")
    readonly property string problem: Scenes.nameProblem(field.text, except)

    signal nameChosen(string name)

    function openWith(initial) {
        field.text = initial
        open()
        field.selectAll()
        field.forceActiveFocus()
    }

    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.NoButton
    customFooterActions: [
        Kirigami.Action {
            text: dialog.actionText
            icon.name: "dialog-ok"
            enabled: dialog.problem === ""
            onTriggered: {
                dialog.nameChosen(field.text.trim())
                dialog.close()
            }
        },
        Kirigami.Action {
            text: i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            onTriggered: dialog.close()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing
        QQC2.TextField {
            id: field
            Layout.fillWidth: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 18
            placeholderText: i18nc("@info:placeholder", "Ranked, Just Chatting, BRB…")
            Accessible.name: i18nc("@label accessible", "Scene name")
            onAccepted: if (dialog.problem === "") {
                dialog.nameChosen(text.trim())
                dialog.close()
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: field.text.length > 0 && dialog.problem !== ""
            text: dialog.problem
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }
    }
}
