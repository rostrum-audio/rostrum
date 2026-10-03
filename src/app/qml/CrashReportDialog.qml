import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// After a crash (mode "ask"): offer to send the report, showing exactly what would go out.
// With example set: the same view for an example report, from the privacy explanation.
Kirigami.Dialog {
    id: dialog

    property bool example: false
    property bool showReport: example
    property string reportText

    function openExample() {
        example = true
        showReport = true
        reportText = CrashReports.exampleReportText()
        open()
    }
    function openPending() {
        example = false
        showReport = false
        reportText = CrashReports.pendingReportText()
        open()
    }

    title: example ? i18nc("@title:dialog", "What a Crash Report Contains")
                   : i18nc("@title:dialog", "Rostrum Quit Unexpectedly")
    preferredWidth: Kirigami.Units.gridUnit * 32
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.NoButton

    customFooterActions: example ? closeActions : askActions
    property list<Kirigami.Action> closeActions: [
        Kirigami.Action {
            text: i18nc("@action:button", "Close")
            icon.name: "dialog-close"
            onTriggered: dialog.close()
        }
    ]
    property list<Kirigami.Action> askActions: [
        Kirigami.Action {
            text: i18nc("@action:button", "Send Report")
            icon.name: "document-send"
            onTriggered: {
                if (alwaysSend.checked) {
                    CrashReports.mode = "send"
                }
                CrashReports.sendPending()
                dialog.close()
            }
        },
        Kirigami.Action {
            text: i18nc("@action:button", "Don't Send")
            icon.name: "dialog-cancel"
            onTriggered: {
                CrashReports.discardPending()
                dialog.close()
            }
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.example
                  ? i18n("This is the whole report, built from this computer with a made-up crash. Folder paths, names and IDs are removed before anything is sent. The memory addresses change every time Rostrum starts, so they say nothing about you.")
                  : CrashReports.pendingCount > 1
                    ? i18n("Rostrum crashed %1 times recently, most recently on %2. Your mix kept playing. Sending the reports helps fix the problem. They contain no personal information.",
                           CrashReports.pendingCount, CrashReports.lastCrashDate)
                    : i18n("Rostrum crashed on %1. Your mix kept playing. Sending the report helps fix the problem. It contains no personal information.",
                           CrashReports.lastCrashDate)
        }

        QQC2.Button {
            visible: !dialog.example
            flat: true
            icon.name: dialog.showReport ? "go-up" : "go-down"
            text: dialog.showReport ? i18nc("@action:button", "Hide the Report") : i18nc("@action:button", "Show the Report")
            onClicked: dialog.showReport = !dialog.showReport
        }

        QQC2.ScrollView {
            visible: dialog.showReport
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 14
            QQC2.TextArea {
                readOnly: true
                text: dialog.reportText
                font.family: "monospace"
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                wrapMode: Text.NoWrap
                selectByMouse: true
                Accessible.name: i18nc("@label accessible", "Crash report")
            }
        }

        QQC2.CheckBox {
            id: alwaysSend
            visible: !dialog.example
            text: i18nc("@option:check", "Send reports without asking from now on")
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            opacity: 0.7
            text: i18n("Reports go to Sentry, a crash reporting service, at %1 over an encrypted connection. Change this any time in Settings → Privacy.",
                       CrashReports.destination)
        }
    }
}
