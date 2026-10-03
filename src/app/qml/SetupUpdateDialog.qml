import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// Shown once to people who finished setup before some of its steps existed: crash reports and
// updates (setup version 2), then OBS (3). Only the steps this user has not seen are shown. A
// dialog, not the full setup, so the Mixer stays usable. Closing it keeps the defaults shown.
FormCard.FormCardDialog {
    id: dialog

    signal exampleRequested()

    // Read when the dialog opens: closing it raises App.setupVersion.
    property int seen: 0
    readonly property bool showPrivacy: seen < 2
    readonly property bool showObs: seen < 3

    title: showPrivacy ? (CrashReports.available ? i18nc("@title:dialog", "Crash Reports, Updates and OBS")
                                                 : i18nc("@title:dialog", "Updates and OBS"))
                       : i18nc("@title:dialog", "Rostrum and OBS")
    width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 36)
    standardButtons: QQC2.Dialog.Ok
    closePolicy: QQC2.Popup.CloseOnEscape
    onAboutToShow: seen = App.setupVersion
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
                text: !dialog.showPrivacy
                      ? i18n("Rostrum can now follow OBS while it runs: LIVE and REC in the header, a warning when a stream starts with your mic muted, and Rostrum scenes that follow OBS scenes. You can change this later in Settings.")
                      : CrashReports.available
                        ? i18n("Rostrum can now help fix crashes, keep itself up to date and follow OBS while it runs. Choose what you are comfortable with; you can change it later in Settings.")
                        : i18n("Rostrum can now keep itself up to date and follow OBS while it runs. You can change this later in Settings.")
            }
            PrivacyChoices {
                visible: dialog.showPrivacy
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                onExampleRequested: dialog.exampleRequested()
            }
            ColumnLayout {
                visible: dialog.showObs
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.bottomMargin: Kirigami.Units.largeSpacing
                spacing: 0

                FormCard.FormHeader {
                    title: i18nc("@title:group", "OBS")
                    maximumWidth: width - 2
                }
                ObsChoices {
                    Layout.fillWidth: true
                }
                FormCard.FormCard {
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    maximumWidth: width - 2
                    FormCard.FormButtonDelegate {
                        text: i18nc("@action:button", "Open the OBS Page")
                        description: i18n("Set OBS up to record Rostrum in one click, and pick a Rostrum scene for each OBS scene.")
                        icon.name: "media-record"
                        onClicked: {
                            dialog.close()
                            applicationWindow().showPage("obs")
                        }
                    }
                }
            }
        }
    }
}
