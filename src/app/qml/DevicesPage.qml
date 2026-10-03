import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Headphone output and mic input pickers. Input meters run only while this page is shown.
QQC2.ScrollView {
    id: page

    contentWidth: availableWidth
    QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

    // The wizard drives the same meters; the two are never on screen together.
    readonly property bool wantMeters: page.visible && page.Window.window !== null && page.Window.window.visible
    onWantMetersChanged: Devices.metersActive = wantMeters

    ColumnLayout {
        width: page.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.bottomMargin: 0
            visible: Devices.anyHeadsetProfile
            type: Kirigami.MessageType.Information
            text: i18n("A Bluetooth headset is in headset mode (HSP/HFP), which sounds muffled. Switch it to A2DP in the desktop sound settings when you do not need its mic.")
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Open Sound Settings")
                    icon.name: "audio-volume-high"
                    onTriggered: Devices.openSoundSettings()
                }
            ]
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            columns: page.availableWidth > Kirigami.Units.gridUnit * 40 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing * 2
            rowSpacing: Kirigami.Units.largeSpacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                Kirigami.Heading {
                    level: 2
                    text: i18nc("@title", "Headphones")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    text: i18n("Where you hear the mix. Buses set to Headphones or Both play here.")
                }
                DeviceList {
                    Layout.fillWidth: true
                    groupName: i18nc("@title", "Headphones")
                    devices: Devices.outputs
                    selected: Devices.headphones
                    inUse: Devices.headphonesInUse
                    onPicked: name => Devices.headphones = name
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                Kirigami.Heading {
                    level: 2
                    text: i18nc("@title", "Mic")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    text: i18n("Speak and watch the meters to find the right one.")
                }
                DeviceList {
                    Layout.fillWidth: true
                    groupName: i18nc("@title", "Mic")
                    isInput: true
                    devices: Devices.inputs
                    selected: Devices.mic
                    inUse: Devices.micInUse
                    levels: Devices.inputLevels
                    onPicked: name => Devices.mic = name
                }
                QQC2.Switch {
                    id: micFallbackSwitch
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    text: i18nc("@option:check", "Use another mic while mine is unplugged")
                    checked: Devices.micFallback
                    onToggled: Devices.micFallback = checked
                    Accessible.description: micFallbackHint.text
                }
                QQC2.Label {
                    id: micFallbackHint
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: Devices.micFallback
                          ? i18n("If your mic is unplugged, the system default mic goes to stream until it comes back. That can be a webcam or laptop mic.")
                          : i18n("If your mic is unplugged, the stream mic stays silent until it comes back, so no other mic goes live by surprise.")
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        QQC2.ToolButton {
            id: advancedToggle
            Layout.leftMargin: Kirigami.Units.largeSpacing
            checkable: true
            text: i18nc("@action:button", "Advanced nodes")
            icon.name: checked ? "arrow-down" : "arrow-right"
            Accessible.description: checked ? i18n("Expanded") : i18n("Collapsed")
        }

        ColumnLayout {
            visible: advancedToggle.checked
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            spacing: 0

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.7
                text: Devices.virtualNodes.length > 0
                      ? i18n("The virtual devices Rostrum created. Other apps can use these names.")
                      : i18n("Rostrum has not created its virtual devices yet.")
            }

            Repeater {
                model: Devices.virtualNodes
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.largeSpacing
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label {
                            text: modelData.description
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            text: Preferences.showNodeIds ? i18nc("@info node name and id", "%1 (#%2)", modelData.name, modelData.id)
                                                          : modelData.name
                            font.family: "monospace"
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            opacity: 0.7
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                    QQC2.ToolButton {
                        icon.name: "edit-copy"
                        text: i18nc("@action:button", "Copy name")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: App.copyToClipboard(modelData.name, i18n("Copied node name"))
                        Accessible.name: i18nc("@action:button accessible", "Copy %1", modelData.name)
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: text
                    }
                }
            }
        }
    }
}
