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

    // Session-only dismiss for the Bluetooth Stream-only hint; shows again next launch if needed.
    property bool streamOnlyBtHintDismissed: false

    // Strips share the row between a narrow and a wide width; below the narrow one the row scrolls.
    // Only strips that show a voice chat fader take the wider size.
    function stripWidthFor(chatShown) {
        const narrow = Kirigami.Units.gridUnit * (chatShown ? 11.5 : 8)
        const wide = Kirigami.Units.gridUnit * (chatShown ? 12 : 10)
        const n = strips.count
        if (n === 0)
            return narrow
        const room = strips.width - (strips.footerItem ? strips.footerItem.width : 0) - (n - 1) * strips.spacing
        return Math.max(narrow, Math.min(wide, Math.floor(room / n)))
    }

    // Meters only run while something shows them: this page, or the compact window, which has
    // meters of its own and hides this page. One owner, so two bindings never disagree.
    Binding {
        target: Mixer
        property: "metersActive"
        value: (page.visible || (page.Window.window !== null && page.Window.window.compact === true))
               && App.mixReady && page.Window.window !== null && page.Window.window.visible
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

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            MasterFader {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                label: i18nc("@label master", "Headphones")
                caption: i18nc("@label master caption", "What you hear")
                iconName: "audio-headphones"
                value: Mixer.masterPhones
                muted: Mixer.masterPhonesMuted
                peak: Mixer.phonesPeak
                clip: Mixer.phonesClip
                onEdited: v => Mixer.masterPhones = v
                onMuteToggled: Mixer.masterPhonesMuted = !Mixer.masterPhonesMuted
            }
            MasterFader {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                label: i18nc("@label master", "Stream")
                caption: i18nc("@label master caption", "What viewers hear")
                value: Mixer.masterStream
                muted: Mixer.masterStreamMuted
                peak: Mixer.streamPeak
                clip: Mixer.streamClip
                onEdited: v => Mixer.masterStream = v
                onMuteToggled: Mixer.masterStreamMuted = !Mixer.masterStreamMuted
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: Mixer.perMixUnavailable.length > 0
            type: Kirigami.MessageType.Warning
            text: Mixer.perMixUnavailable
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !page.streamOnlyBtHintDismissed
                     && Devices.headphonesAreBluetooth
                     && Mixer.streamOnlyPlaybackBusNames.length > 0
            type: Kirigami.MessageType.Information
            text: {
                const names = Mixer.streamOnlyPlaybackBusNames
                if (names.length === 1)
                    return i18n("%1 is set to Stream only, so it will not play in your headphones.", names[0])
                return i18n("%1 are set to Stream only, so they will not play in your headphones.",
                            names.join(i18nc("list separator", ", ")))
            }
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Include in headphones")
                    icon.name: "audio-headphones"
                    onTriggered: Mixer.includeStreamOnlyBusesInHeadphones()
                },
                Kirigami.Action {
                    text: i18nc("@action:button", "Dismiss")
                    onTriggered: page.streamOnlyBtHintDismissed = true
                }
            ]
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
            spacing: Kirigami.Units.smallSpacing * 2
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
                width: page.stripWidthFor(chat && chatAllowed)
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

    ConfirmDialog {
        id: removeDialog
        objectName: "removeBusDialog"
        property string busId
        property string busName
        property int assigned: 0
        title: i18nc("@title:dialog", "Remove %1?", busName)
        subtitle: assigned > 0
                  ? i18ncp("@info", "One app assigned to %2 goes back to the default output.",
                           "%1 apps assigned to %2 go back to the default output.", assigned, busName)
                  : i18n("The %1 bus is removed from this scene.", busName)
        confirmText: i18nc("@action:button", "Remove")
        confirmIconName: "edit-delete"
        onConfirmed: {
            Mixer.remove(removeDialog.busId)
            removeDialog.close()
        }
    }
}
