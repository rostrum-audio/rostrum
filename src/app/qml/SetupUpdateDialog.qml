import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// Shown once to people who finished setup before crash reports and updates existed. A dialog,
// not the full setup, so the Mixer stays usable. Closing it keeps the defaults shown.
FormCard.FormCardDialog {
    id: dialog

    signal exampleRequested()

    title: i18nc("@title:dialog", "Crash Reports and Updates")
    width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 36)
    standardButtons: QQC2.Dialog.Ok
    closePolicy: QQC2.Popup.CloseOnEscape
    onClosed: App.finishSetupUpdate()
    Component.onCompleted: standardButton(QQC2.Dialog.Ok).text = i18nc("@action:button", "Done")

    // A fixed-size scroll area keeps the dialog's height independent of how the text wraps.
    QQC2.ScrollView {
        id: scroll
        Layout.fillWidth: true
        implicitHeight: Math.min(dialog.parent.height - Kirigami.Units.gridUnit * 9, Kirigami.Units.gridUnit * 32)
        contentWidth: availableWidth
        QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
        QQC2.ScrollBar.vertical.policy: QQC2.ScrollBar.AlwaysOn

        ColumnLayout {
            width: scroll.availableWidth
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                Layout.bottomMargin: 0
                wrapMode: Text.WordWrap
                text: i18n("Rostrum can now help fix crashes and keep itself up to date. Choose what you are comfortable with; you can change it later in Settings.")
            }
            PrivacyChoices {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.bottomMargin: Kirigami.Units.largeSpacing
                onExampleRequested: dialog.exampleRequested()
            }
        }
    }
}
