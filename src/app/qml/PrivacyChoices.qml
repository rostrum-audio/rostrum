import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// Crash reports and updates, shared by first-run setup and the one-time dialog for people who
// set Rostrum up before these choices existed. Choices apply as soon as they are made.
ColumnLayout {
    id: root

    signal exampleRequested()

    spacing: 0

    FormCard.FormHeader {
        visible: CrashReports.available
        title: i18nc("@title:group", "Crash reports")
        maximumWidth: width - 2
    }
    FormCard.FormCard {
        visible: CrashReports.available
        maximumWidth: width - 2
        FormCard.FormRadioDelegate {
            text: i18nc("@option:radio crash reports", "Send automatically")
            description: i18n("After a crash, a short report goes out quietly the next time Rostrum starts.")
            checked: CrashReports.mode === "send"
            onToggled: if (checked) CrashReports.mode = "send"
        }
        FormCard.FormRadioDelegate {
            text: i18nc("@option:radio crash reports", "Ask me after a crash")
            description: i18n("You see the report and decide each time.")
            checked: CrashReports.mode === "ask"
            onToggled: if (checked) CrashReports.mode = "ask"
        }
        FormCard.FormRadioDelegate {
            text: i18nc("@option:radio crash reports", "Never send")
            description: i18n("Crashes are not recorded, and nothing leaves this computer.")
            checked: CrashReports.mode === "never"
            onToggled: if (checked) CrashReports.mode = "never"
        }
    }

    // The promise, spelled out next to the choice it applies to.
    Rectangle {
        visible: CrashReports.available
        Layout.fillWidth: true
        Layout.leftMargin: 1
        Layout.rightMargin: 1
        Layout.topMargin: Kirigami.Units.largeSpacing
        implicitHeight: promise.implicitHeight + Kirigami.Units.largeSpacing * 2
        radius: Kirigami.Units.cornerRadius
        color: Qt.alpha(Kirigami.Theme.positiveTextColor, 0.08)
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.positiveTextColor, 0.35)

        RowLayout {
            id: promise
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                Layout.alignment: Qt.AlignTop
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: implicitWidth
                source: "security-high"
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18nc("@title", "No personal information, ever")
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("A report holds only what is needed to find the bug, and goes to Sentry, a crash reporting service. There is no account and no ID that links reports to you or to each other.")
                    wrapMode: Text.WordWrap
                    opacity: 0.85
                }
                GridLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    columns: 2
                    columnSpacing: Kirigami.Units.gridUnit
                    rowSpacing: Kirigami.Units.smallSpacing

                    QQC2.Label {
                        text: i18nc("@title crash report contents", "Included")
                        font: Kirigami.Theme.smallFont
                        color: Kirigami.Theme.positiveTextColor
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                    }
                    QQC2.Label {
                        text: i18nc("@title crash report contents", "Never included")
                        font: Kirigami.Theme.smallFont
                        color: Kirigami.Theme.negativeTextColor
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.alignment: Qt.AlignTop
                        wrapMode: Text.WordWrap
                        font: Kirigami.Theme.smallFont
                        text: [i18n("Where in Rostrum's code it crashed, and the libraries on the way there"),
                               i18n("Rostrum's version, build and how it was installed"),
                               i18n("Linux distribution, kernel, CPU type, desktop and session type"),
                               i18n("Qt, KDE Frameworks, PipeWire and WirePlumber versions")].map(s => "• " + s).join("\n")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.alignment: Qt.AlignTop
                        wrapMode: Text.WordWrap
                        font: Kirigami.Theme.smallFont
                        text: [i18n("Your name, user name, files or folders"),
                               i18n("App, device and scene names"),
                               i18n("Logs, audio, or anything you type or say"),
                               i18n("Tracking IDs or accounts")].map(s => "• " + s).join("\n")
                    }
                }
                QQC2.Button {
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    flat: true
                    icon.name: "document-preview"
                    text: i18nc("@action:button", "See an Example Report")
                    onClicked: root.exampleRequested()
                }
            }
        }
    }

    // Alone on the page, the updates card needs no heading of its own.
    FormCard.FormHeader {
        visible: CrashReports.available
        title: i18nc("@title:group", "Updates")
        maximumWidth: width - 2
    }
    FormCard.FormCard {
        maximumWidth: width - 2
        FormCard.FormSwitchDelegate {
            visible: Updates.canCheck
            text: i18nc("@option:check", "Check for updates")
            description: i18n("Once a day, Rostrum asks %1 for the newest version number. Nothing about you is sent.", Updates.feedHost)
            checked: Updates.checkEnabled
            onToggled: Updates.checkEnabled = checked
        }
        FormCard.FormDelegateSeparator {
            visible: Updates.canCheck
        }
        FormCard.FormSwitchDelegate {
            visible: Updates.canCheck
            text: i18nc("@option:check", "Install updates automatically")
            enabled: Updates.canInstall && Updates.checkEnabled
            checked: Updates.canInstall && Updates.autoInstall
            onToggled: Updates.autoInstall = checked
            description: Updates.installKind === "appimage"
                         ? (Updates.canInstall ? i18n("New versions download in the background, are checked against the release checksum, and start the next time you open Rostrum.")
                                               : i18n("Rostrum cannot write to the folder its AppImage is in, so it only tells you about new versions."))
                         : Updates.installKind === "package"
                           ? i18n("Your package manager installs Rostrum's updates. Rostrum tells you when one is out.")
                           : i18n("This copy was built from source, so Rostrum tells you when a new version is out and links to what changed.")
        }
        FormCard.FormTextDelegate {
            visible: !Updates.canCheck
            text: i18nc("@label", "Updates")
            description: i18n("Flatpak keeps Rostrum up to date through Discover or your software center.")
            textItem.wrapMode: Text.WordWrap
        }
    }
}
