import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A question before something is removed, overwritten or interrupted. Cancel is the default: it
// has the focus when the dialog opens, so Enter and Escape both leave things as they are. The
// confirming button is drawn as destructive unless `destructive` is off.
Kirigami.PromptDialog {
    id: dialog

    property string confirmText
    property string confirmIconName
    property bool destructive: true
    // Other choices, shown before the confirming button.
    property list<Kirigami.Action> extraActions

    // The dialog stays open; the handler closes it when it is done.
    signal confirmed()

    standardButtons: Kirigami.Dialog.NoButton
    onOpened: cancelButton.forceActiveFocus(Qt.TabFocusReason)

    footer: QQC2.Control {
        padding: Kirigami.Units.largeSpacing
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            // No clicks while the dialog fades in or out.
            enabled: dialog.opened
            Item {
                Layout.fillWidth: true
            }
            Repeater {
                model: dialog.extraActions
                delegate: QQC2.Button {
                    required property Kirigami.Action modelData
                    action: modelData
                }
            }
            DestructiveButton {
                objectName: "confirmButton"
                visible: dialog.destructive
                text: dialog.confirmText
                icon.name: dialog.confirmIconName
                onClicked: dialog.confirmed()
            }
            QQC2.Button {
                objectName: "confirmPlainButton"
                visible: !dialog.destructive
                text: dialog.confirmText
                icon.name: dialog.confirmIconName
                onClicked: dialog.confirmed()
            }
            QQC2.Button {
                id: cancelButton
                objectName: "cancelButton"
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                highlighted: true
                onClicked: dialog.close()
            }
        }
    }
}
