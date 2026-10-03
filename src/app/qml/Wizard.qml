import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// First run: Welcome, Headphones, Mic, Buses. Skip still creates the mix with the defaults.
// "Create mix" stays on the last step and shows the PipeWire error if node creation fails.
QQC2.Pane {
    id: wizard

    property int step: 0
    property bool creating: false
    readonly property var titles: [i18nc("@title wizard step", "Welcome"),
                                   i18nc("@title wizard step", "Headphones"),
                                   i18nc("@title wizard step", "Mic"),
                                   i18nc("@title wizard step", "Buses")]

    readonly property bool wantMeters: visible && step === 2
    onWantMetersChanged: Devices.metersActive = wantMeters

    // The mix is ready once every node exists; then the wizard lands on the Mixer.
    Connections {
        target: App
        function onMixChanged() {
            if (wizard.creating && App.mixReady) {
                wizard.creating = false
                App.finishWizard()
            } else if (wizard.creating && App.mixError !== "") {
                wizard.creating = false
            }
        }
    }

    function createMix() {
        creating = true
        App.createMix()
        if (App.mixReady) {
            creating = false
            App.finishWizard()
        }
    }

    focusPolicy: Qt.NoFocus
    padding: Kirigami.Units.gridUnit

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width, Kirigami.Units.gridUnit * 36)
        height: Math.min(parent.height, implicitHeight)
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            text: i18nc("@info wizard progress", "Step %1 of %2: %3", wizard.step + 1, 4, wizard.titles[wizard.step])
            opacity: 0.7
            Accessible.role: Accessible.StaticText
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentItem: StackLayout {
                currentIndex: wizard.step
                implicitHeight: children[currentIndex] ? children[currentIndex].implicitHeight : 0

                // 1. Welcome
                ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading {
                        level: 1
                        text: i18nc("@title", "Set up your stream mix")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("Rostrum puts each app on a bus, and sends each bus to your headphones, your stream, or both.")
                    }
                    MixDiagram {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: Kirigami.Units.largeSpacing
                        Layout.bottomMargin: Kirigami.Units.largeSpacing
                    }
                }

                // 2. Headphones
                ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading {
                        level: 1
                        text: i18nc("@title", "Where do you listen?")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("Pick your headphones. Press Test to hear a short tone.")
                    }
                    QQC2.ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(outputList.implicitHeight, Kirigami.Units.gridUnit * 14)
                        DeviceList {
                            id: outputList
                            width: parent.width
                            groupName: i18nc("@title", "Headphones")
                            devices: Devices.outputs
                            selected: Devices.headphones
                            inUse: Devices.headphonesInUse
                            onPicked: name => Devices.headphones = name
                        }
                    }
                }

                // 3. Mic
                ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading {
                        level: 1
                        text: i18nc("@title", "Which mic is yours?")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("Say something. The meter under the right mic moves.")
                    }
                    QQC2.ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(inputList.implicitHeight, Kirigami.Units.gridUnit * 12)
                        DeviceList {
                            id: inputList
                            width: parent.width
                            groupName: i18nc("@title", "Mic")
                            isInput: true
                            devices: Devices.inputs
                            selected: Devices.mic
                            inUse: Devices.micInUse
                            levels: Devices.inputLevels
                            onPicked: name => Devices.mic = name
                        }
                    }
                    RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        QQC2.Button {
                            checkable: true
                            checked: App.micMuted
                            icon.name: App.micMuted ? "microphone-sensitivity-muted" : "audio-input-microphone"
                            text: App.micMuted ? i18nc("@action:button", "Unmute Mic") : i18nc("@action:button", "Mute Mic")
                            onToggled: App.micMuted = checked
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            opacity: 0.7
                            text: i18n("The big mic button in the header does the same, any time. Ctrl+M too.")
                        }
                    }
                }

                // 4. Buses
                ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading {
                        level: 1
                        text: i18nc("@title", "Your buses")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("These are ready to go. You can rename, recolor and reroute them on the Mixer.")
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing * 2
                        rowSpacing: Kirigami.Units.smallSpacing
                        Repeater {
                            model: Mixer.buses
                            delegate: RowLayout {
                                required property string name
                                required property color busColor
                                required property int destination
                                required property bool isInput
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing
                                Rectangle {
                                    implicitWidth: 4
                                    implicitHeight: Kirigami.Units.gridUnit
                                    radius: 2
                                    color: busColor
                                }
                                QQC2.Label {
                                    text: name
                                    font.weight: Font.DemiBold
                                    Layout.fillWidth: true
                                }
                                QQC2.Label {
                                    text: destination === 0 ? i18nc("destination", "Phones")
                                        : destination === 1 ? i18nc("destination", "Stream")
                                        : i18nc("destination", "Both")
                                    opacity: 0.7
                                }
                            }
                        }
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: App.mixError !== "" && !wizard.creating
                        type: Kirigami.MessageType.Error
                        text: i18n("PipeWire did not create the mix: %1 Check that PipeWire and WirePlumber are running, then try again.",
                                   App.mixError)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            QQC2.Button {
                visible: wizard.step === 0
                text: i18nc("@action:button", "Skip")
                onClicked: App.skipWizard()
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Create the mix with the defaults and go to the Mixer")
            }
            QQC2.Button {
                visible: wizard.step > 0
                enabled: !wizard.creating
                text: i18nc("@action:button", "Back")
                icon.name: "go-previous"
                onClicked: wizard.step -= 1
            }
            Item {
                Layout.fillWidth: true
            }
            QQC2.BusyIndicator {
                visible: wizard.creating
                running: visible
                implicitHeight: createButton.implicitHeight
                implicitWidth: implicitHeight
                Accessible.name: i18n("Creating the mix")
            }
            QQC2.Button {
                id: nextButton
                visible: wizard.step < 3
                highlighted: true
                text: wizard.step === 0 ? i18nc("@action:button", "Start") : i18nc("@action:button", "Next")
                icon.name: "go-next"
                onClicked: wizard.step += 1
            }
            QQC2.Button {
                id: createButton
                visible: wizard.step === 3
                highlighted: true
                enabled: !wizard.creating
                text: wizard.creating ? i18nc("@action:button", "Creating…") : i18nc("@action:button", "Create Mix")
                icon.name: "dialog-ok-apply"
                onClicked: wizard.createMix()
            }
        }
    }

    onStepChanged: nextButton.visible ? nextButton.forceActiveFocus() : createButton.forceActiveFocus()
    Component.onCompleted: nextButton.forceActiveFocus()
}
