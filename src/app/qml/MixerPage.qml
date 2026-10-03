import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Home: masters on top, then the strip row. Dropping an app chip on the background unassigns it.
QQC2.Pane {
    id: page

    padding: Kirigami.Units.largeSpacing
    focusPolicy: Qt.NoFocus

    Binding {
        target: Mixer
        property: "metersActive"
        value: page.visible && App.mixReady && page.Window.window !== null && page.Window.window.visible
    }

    DropArea {
        anchors.fill: parent
        keys: ["application/x-rostrum-app"]
        onDropped: drop => {
            const key = drop.getDataAsString("application/x-rostrum-app")
            if (key) {
                Mixer.unassignApp(key)
                drop.acceptProposedAction()
            }
        }
    }

    // Before the first run creates the mix, offer to create it here too.
    ColumnLayout {
        anchors.centerIn: parent
        visible: !App.mixEnabled
        width: Math.min(parent.width, Kirigami.Units.gridUnit * 26)

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            icon.name: "view-media-equalizer"
            text: i18nc("@title", "No mix yet")
            explanation: i18n("Create the mix to add Rostrum's buses to PipeWire.")
        }
        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            text: i18nc("@action:button", "Create Mix")
            icon.name: "list-add"
            onClicked: App.createMix()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        visible: App.mixEnabled
        spacing: Kirigami.Units.largeSpacing

        QQC2.Frame {
            Layout.fillWidth: true
            padding: Kirigami.Units.largeSpacing

            RowLayout {
                anchors.fill: parent
                spacing: Kirigami.Units.gridUnit

                MasterFader {
                    Layout.fillWidth: true
                    label: i18nc("@label master", "Phones")
                    iconName: "audio-headphones"
                    value: Mixer.masterPhones
                    muted: Mixer.masterPhonesMuted
                    peak: Mixer.phonesPeak
                    clip: Mixer.phonesClip
                    onEdited: v => Mixer.masterPhones = v
                    onMuteToggled: Mixer.masterPhonesMuted = !Mixer.masterPhonesMuted
                }
                Kirigami.Separator {
                    Layout.fillHeight: true
                }
                MasterFader {
                    Layout.fillWidth: true
                    label: i18nc("@label master", "Stream")
                    iconName: "media-record"
                    value: Mixer.masterStream
                    muted: Mixer.masterStreamMuted
                    peak: Mixer.streamPeak
                    clip: Mixer.streamClip
                    onEdited: v => Mixer.masterStream = v
                    onMuteToggled: Mixer.masterStreamMuted = !Mixer.masterStreamMuted
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: App.mixBusy
            QQC2.BusyIndicator {
                running: App.mixBusy
                implicitHeight: Kirigami.Units.gridUnit * 1.5
                implicitWidth: implicitHeight
            }
            QQC2.Label {
                text: i18n("Creating the mix in PipeWire…")
            }
        }

        ListView {
            id: strips
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: ListView.Horizontal
            spacing: Kirigami.Units.smallSpacing
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: Mixer.buses
            activeFocusOnTab: false
            keyNavigationEnabled: false
            reuseItems: false
            QQC2.ScrollBar.horizontal: QQC2.ScrollBar {
                policy: QQC2.ScrollBar.AsNeeded
            }

            delegate: BusStrip {
                height: ListView.view.height - (strips.QQC2.ScrollBar.horizontal.visible ? strips.QQC2.ScrollBar.horizontal.height : 0)
                onRemoveRequested: (busId, name) => {
                    removeDialog.busId = busId
                    removeDialog.busName = name
                    removeDialog.assigned = Mixer.assignedCount(busId)
                    removeDialog.open()
                }
                onOverflowRequested: busId => {
                    App.appsFilter = busId
                    applicationWindow().showPage("apps")
                }
            }

            footer: Item {
                width: addButton.width + Kirigami.Units.largeSpacing * 2
                height: strips.height
                QQC2.ToolButton {
                    id: addButton
                    anchors.centerIn: parent
                    visible: Mixer.canAddBus
                    icon.name: "list-add"
                    text: i18nc("@action:button", "Add Bus")
                    display: QQC2.AbstractButton.IconOnly
                    opacity: hovered || activeFocus ? 1 : 0.6
                    onClicked: {
                        const id = Mixer.addBus()
                        if (id.length > 0) {
                            Qt.callLater(() => strips.positionViewAtEnd())
                        }
                    }
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }
    }

    Kirigami.PromptDialog {
        id: removeDialog
        property string busId
        property string busName
        property int assigned: 0
        title: i18nc("@title:dialog", "Remove %1?", busName)
        subtitle: assigned > 0
                  ? i18ncp("@info", "One app assigned to %2 goes back to the default output.",
                           "%1 apps assigned to %2 go back to the default output.", assigned, busName)
                  : i18n("The %1 bus is removed from this scene.", busName)
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Remove")
                icon.name: "edit-delete"
                onTriggered: {
                    Mixer.remove(removeDialog.busId)
                    removeDialog.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: removeDialog.close()
            }
        ]
    }
}
