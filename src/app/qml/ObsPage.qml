import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// A checklist with copy buttons, not a settings form. No websocket: OBS is optional.
QQC2.ScrollView {
    id: page

    contentWidth: availableWidth
    QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

    readonly property var streamNode: Devices.virtualNodes.find(n => n.name === "rostrum.stream") ?? null
    readonly property var micNode: Devices.virtualNodes.find(n => n.name === "rostrum.mic") ?? null
    readonly property bool nodesPresent: streamNode !== null && micNode !== null

    component NodeName: RowLayout {
        id: nodeRow
        property string label
        property string nodeName
        property bool present
        spacing: Kirigami.Units.smallSpacing
        Kirigami.Icon {
            source: nodeRow.present ? "checkmark" : "dialog-warning"
            color: nodeRow.present ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
            Accessible.name: nodeRow.present ? i18n("Present") : i18n("Missing")
        }
        QQC2.Label {
            text: nodeRow.label
            font.weight: Font.DemiBold
        }
        QQC2.Label {
            text: i18nc("@info node name in parentheses", "(%1)", nodeRow.nodeName)
            font.family: "monospace"
            opacity: 0.7
        }
        QQC2.Button {
            text: i18nc("@action:button", "Copy")
            icon.name: "edit-copy"
            onClicked: App.copyToClipboard(nodeRow.label, i18n("Copied node name"))
            Accessible.name: i18nc("@action:button accessible", "Copy %1", nodeRow.label)
        }
    }

    component Step: RowLayout {
        id: step
        property int number
        default property alias content: body.data
        Layout.fillWidth: true
        spacing: Kirigami.Units.largeSpacing
        Rectangle {
            Layout.alignment: Qt.AlignTop
            implicitWidth: Kirigami.Units.gridUnit * 1.5
            implicitHeight: implicitWidth
            radius: width / 2
            color: Kirigami.Theme.highlightColor
            QQC2.Label {
                anchors.centerIn: parent
                text: step.number
                color: Kirigami.Theme.highlightedTextColor
                font.weight: Font.Bold
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: i18nc("@info accessible", "Step %1", step.number)
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
        }
    }

    ColumnLayout {
        width: Math.min(page.availableWidth, Kirigami.Units.gridUnit * 42)
        x: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing * 2

        Item {
            implicitHeight: Kirigami.Units.smallSpacing
        }

        Kirigami.Heading {
            level: 1
            text: i18nc("@title", "Add Rostrum to OBS")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !page.nodesPresent
            type: Kirigami.MessageType.Warning
            text: App.mixBusy ? i18n("Creating the mix in PipeWire…")
                              : i18n("The Rostrum nodes are not there yet. Create the mix first.")
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Create Mix")
                    icon.name: "list-add"
                    enabled: !App.mixBusy
                    onTriggered: App.createMix()
                }
            ]
        }

        Step {
            number: 1
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18n("In OBS, add a source: <b>Audio Capture (PipeWire)</b>, or <b>Application Audio Capture (PipeWire)</b>.")
                textFormat: Text.StyledText
            }
        }

        Step {
            number: 2
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18n("Pick this device for the game, music and desktop mix:")
            }
            NodeName {
                label: "Rostrum Stream Mix"
                nodeName: "rostrum.stream"
                present: page.streamNode !== null
            }
        }

        Step {
            number: 3
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18n("Add another source and pick this device for your microphone:")
            }
            NodeName {
                label: "Rostrum Mic"
                nodeName: "rostrum.mic"
                present: page.micNode !== null
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.8
                text: i18n("Do not also capture your desktop mic in OBS, or your voice will be doubled.")
            }
        }

        Step {
            number: 4
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18n("Optional, for separate VOD audio: in OBS, open Advanced Audio Properties. Send Rostrum Stream Mix to track 1, and Rostrum Mic to tracks 1 and 2.")
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: true
            type: Kirigami.MessageType.Information
            text: i18n("OBS must capture through PipeWire, not the old PulseAudio monitor of the whole desktop. Otherwise it records everything and Rostrum's split is lost.")
        }

        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            QQC2.Button {
                text: i18nc("@action:button", "Refresh Nodes")
                icon.name: "view-refresh"
                onClicked: Devices.refresh()
            }
            QQC2.Button {
                visible: !page.nodesPresent
                text: i18nc("@action:button", "Create Mix")
                icon.name: "list-add"
                enabled: !App.mixBusy
                highlighted: true
                onClicked: App.createMix()
                QQC2.BusyIndicator {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.right
                    anchors.leftMargin: Kirigami.Units.smallSpacing
                    height: parent.height
                    width: height
                    running: App.mixBusy
                    visible: running
                }
            }
        }

        Item {
            implicitHeight: Kirigami.Units.largeSpacing
        }
    }
}
