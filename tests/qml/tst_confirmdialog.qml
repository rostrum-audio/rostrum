import QtQuick
import QtTest
import org.kde.kirigami as Kirigami
import "../../src/app/qml"

Item {
    id: root
    width: 800
    height: 600

    property int confirmedCount: 0
    property int savedCount: 0

    Component {
        id: destructive
        ConfirmDialog {
            parent: root
            title: "Remove Game?"
            subtitle: "The Game bus is removed from this scene."
            confirmText: "Remove"
            confirmIconName: "edit-delete"
            onConfirmed: {
                root.confirmedCount += 1
                close()
            }
        }
    }
    Component {
        id: plain
        ConfirmDialog {
            parent: root
            title: "Discard fader moves?"
            destructive: false
            confirmText: "Switch"
            extraActions: [
                Kirigami.Action {
                    text: "Save and Switch"
                    onTriggered: root.savedCount += 1
                }
            ]
            onConfirmed: root.confirmedCount += 1
        }
    }

    TestCase {
        name: "ConfirmDialog"
        when: windowShown

        function init() {
            root.confirmedCount = 0
            root.savedCount = 0
        }
        function shown(component) {
            const dialog = createTemporaryObject(component, root)
            verify(dialog)
            dialog.open()
            tryCompare(dialog, "opened", true)
            return dialog
        }
        function test_cancelIsTheDefault() {
            const dialog = shown(destructive)
            const cancel = findChild(dialog.footer, "cancelButton")
            const confirm = findChild(dialog.footer, "confirmButton")
            compare(cancel.text, "Cancel")
            compare(confirm.text, "Remove")
            compare(confirm.Accessible.name, "Remove")
            // Cancel has the focus, so the keyboard's default choice changes nothing.
            tryCompare(cancel, "activeFocus", true)
            compare(confirm.activeFocus, false)
            keyClick(Qt.Key_Space)
            tryCompare(dialog, "visible", false)
            compare(root.confirmedCount, 0)
        }
        function test_escapeCancels() {
            const dialog = shown(destructive)
            keyClick(Qt.Key_Escape)
            tryCompare(dialog, "visible", false)
            compare(root.confirmedCount, 0)
        }
        function test_confirmNeedsItsOwnButton() {
            const dialog = shown(destructive)
            const confirm = findChild(dialog.footer, "confirmButton")
            verify(confirm.visible)
            compare(findChild(dialog.footer, "confirmPlainButton").visible, false)
            mouseClick(confirm)
            compare(root.confirmedCount, 1)
            tryCompare(dialog, "visible", false)
        }
        function test_destructiveButtonWorksFromTheKeyboard() {
            const dialog = shown(destructive)
            const confirm = findChild(dialog.footer, "confirmButton")
            confirm.forceActiveFocus()
            keyClick(Qt.Key_Return)
            compare(root.confirmedCount, 1)
        }
        function test_plainConfirmAndExtraChoices() {
            const dialog = shown(plain)
            const confirm = findChild(dialog.footer, "confirmPlainButton")
            verify(confirm.visible)
            compare(findChild(dialog.footer, "confirmButton").visible, false)
            tryCompare(findChild(dialog.footer, "cancelButton"), "activeFocus", true)
            mouseClick(confirm)
            compare(root.confirmedCount, 1)
            // The dialog stays open until its handler closes it.
            compare(dialog.visible, true)
            dialog.close()
        }
    }
}
