import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Full-page block when PipeWire is unreachable or too old. The sidebar is disabled meanwhile.
Item {
    id: page

    component CommandRow: RowLayout {
        id: row
        required property string command
        spacing: Kirigami.Units.smallSpacing
        QQC2.TextField {
            Layout.fillWidth: true
            text: row.command
            readOnly: true
            selectByMouse: true
            font: Kirigami.Theme.fixedWidthFont
            Accessible.name: i18nc("@label accessible", "Command")
            Component.onCompleted: cursorPosition = 0
        }
        QQC2.ToolButton {
            icon.name: "edit-copy"
            text: i18nc("@action:button", "Copy")
            display: QQC2.AbstractButton.IconOnly
            onClicked: App.copyToClipboard(row.command, i18n("Copied command"))
            QQC2.ToolTip.text: i18nc("@info:tooltip", "Copy command")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }

    component SectionLabel: ColumnLayout {
        id: section
        required property string title
        required property string explanation
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.largeSpacing
        spacing: 0
        Kirigami.Heading {
            level: 4
            text: section.title
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: section.explanation
            wrapMode: Text.WordWrap
            opacity: 0.7
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 32)
        spacing: Kirigami.Units.smallSpacing

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            icon.name: App.pipewireTooOld ? "dialog-warning" : "dialog-error"
            text: App.pipewireTooOld ? i18nc("@title", "PipeWire is too old")
                                     : i18nc("@title", "Rostrum can't reach PipeWire")
            explanation: App.pipewireDetail
        }

        SectionLabel {
            visible: !App.pipewireTooOld
            title: i18nc("@title:group", "Start PipeWire")
            explanation: i18n("It is usually installed already and only needs starting for your session.")
        }
        CommandRow {
            visible: !App.pipewireTooOld
            command: App.startCommand
        }

        SectionLabel {
            title: App.pipewireTooOld ? i18nc("@title:group", "Update PipeWire")
                                      : i18nc("@title:group", "Not installed?")
            explanation: App.pipewireTooOld
                         ? i18n("Rostrum needs PipeWire 1.0 or newer with WirePlumber 0.5 or newer. Install the latest packages, then log out and back in.")
                         : i18n("Install PipeWire with WirePlumber, then log out and back in.")
        }
        Repeater {
            model: App.installCommands
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 2
                QQC2.Label {
                    text: modelData.distro
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                }
                CommandRow {
                    Layout.fillWidth: true
                    command: modelData.command
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Kirigami.Units.largeSpacing * 2
            spacing: Kirigami.Units.largeSpacing
            QQC2.Label {
                visible: !App.pipewireTooOld
                text: i18n("Rostrum checks again every few seconds.")
                opacity: 0.7
            }
            QQC2.Button {
                text: i18nc("@action:button", "Retry Now")
                icon.name: "view-refresh"
                onClicked: App.retry()
            }
        }
    }
}
