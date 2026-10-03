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
    // Shown above the name field when set, e.g. for a preset.
    property string heading
    property string description
    property string iconName
    property string note
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
        RowLayout {
            visible: dialog.description.length > 0
            Layout.fillWidth: true
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing
            Rectangle {
                Layout.alignment: Qt.AlignTop
                implicitWidth: Kirigami.Units.iconSizes.large
                implicitHeight: implicitWidth
                radius: Kirigami.Units.cornerRadius
                color: Qt.alpha(Kirigami.Theme.highlightColor, 0.15)
                Kirigami.Icon {
                    anchors.centerIn: parent
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: implicitWidth
                    source: dialog.iconName
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                QQC2.Label {
                    text: dialog.heading
                    font.weight: Font.DemiBold
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                    text: dialog.description
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                }
            }
        }
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
        QQC2.Label {
            Layout.fillWidth: true
            visible: dialog.note.length > 0 && (field.text.length === 0 || dialog.problem === "")
            text: dialog.note
            font: Kirigami.Theme.smallFont
            opacity: 0.7
            wrapMode: Text.WordWrap
        }
    }
}
