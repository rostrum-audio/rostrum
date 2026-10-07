import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// One button sets OBS up; the rest shows what OBS actually records. The manual steps stay
// for people without obs-websocket or with another recorder.
QQC2.ScrollView {
    id: page

    contentWidth: availableWidth
    QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

    readonly property var streamNode: Devices.virtualNodes.find(n => n.name === "rostrum.stream") ?? null
    readonly property var micNode: Devices.virtualNodes.find(n => n.name === "rostrum.mic") ?? null
    readonly property var vodNode: Devices.virtualNodes.find(n => n.name === "rostrum.vod") ?? null
    readonly property bool nodesPresent: streamNode !== null && micNode !== null && vodNode !== null
    readonly property bool obsRunning: ["noWebSocket", "connecting", "connected", "authFailed", "failed"].includes(Obs.state)
    property bool manualOpen: false

    Binding {
        target: Obs
        property: "active"
        value: page.visible && page.Window.window !== null && page.Window.window.visible
    }

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
        width: Math.min(page.availableWidth - Kirigami.Units.largeSpacing * 2, Kirigami.Units.gridUnit * 54)
        x: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing * 2

        Item {
            implicitHeight: Kirigami.Units.smallSpacing
        }

        Kirigami.Heading {
            level: 1
            text: i18nc("@title", "OBS")
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

        ReadinessPanel {
            Layout.fillWidth: true
            results: Obs.readiness
            checking: Obs.readinessChecking
            enabled: !Obs.busy
            onCheckRequested: Obs.checkReadiness()
            onNavigateRequested: targetPage => App.lastPage = targetPage
            onReviewObsRequested: {
                const maximum = Math.max(0, page.contentItem.contentHeight - page.contentItem.height)
                page.contentItem.contentY = Math.min(maximum, Math.max(0, obsSetupSection.y - Kirigami.Units.largeSpacing))
                obsRefreshButton.forceActiveFocus(Qt.TabFocusReason)
            }
        }

        // Status and the one-click setup
        RowLayout {
            id: obsSetupSection
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Item {
                Layout.alignment: Qt.AlignTop
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: implicitWidth
                QQC2.BusyIndicator {
                    anchors.fill: parent
                    running: Obs.state === "connecting" || Obs.busy
                    visible: running
                }
                Kirigami.Icon {
                    anchors.fill: parent
                    visible: !(Obs.state === "connecting" || Obs.busy)
                    source: Obs.setUp ? "checkmark"
                          : Obs.state === "notInstalled" ? "help-about"
                          : "dialog-warning"
                    color: Obs.setUp ? Kirigami.Theme.positiveTextColor
                         : Obs.state === "notInstalled" ? Kirigami.Theme.textColor
                         : Kirigami.Theme.neutralTextColor
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 3
                    wrapMode: Text.WordWrap
                    text: Obs.summary
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    wrapMode: Text.WordWrap
                    opacity: 0.8
                    text: Obs.detail
                }
                RowLayout {
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.largeSpacing
                    QQC2.Button {
                        visible: !Obs.setUp && Obs.state !== "notInstalled" && Obs.state !== "neverRun"
                        enabled: Obs.canApply && page.nodesPresent
                        highlighted: true
                        icon.name: "configure"
                        text: Obs.onlyDoubling ? i18nc("@action:button", "Fix OBS…")
                                               : i18nc("@action:button", "Set Up OBS…")
                        onClicked: previewDialog.open()
                    }
                    QQC2.Button {
                        visible: Obs.canUndo
                        icon.name: "edit-undo"
                        text: i18nc("@action:button", "Undo OBS Changes")
                        enabled: !Obs.busy
                        onClicked: Obs.undo()
                    }
                    QQC2.Button {
                        id: obsRefreshButton
                        icon.name: "view-refresh"
                        text: i18nc("@action:button", "Check Again")
                        enabled: !Obs.busy
                        onClicked: {
                            Devices.refresh()
                            Obs.refresh()
                        }
                    }
                }
            }
        }

        RecordingTracks {
            Layout.fillWidth: true
            setup: Obs.recordingTracks
        }

        // What OBS records, straight from the PipeWire graph
        ColumnLayout {
            Layout.fillWidth: true
            visible: page.obsRunning
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 3
                text: i18nc("@title", "What OBS records right now")
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: Obs.recordings.length === 0
                wrapMode: Text.WordWrap
                opacity: 0.7
                text: i18n("No audio device. Sources that aren't playing anything don't show up here.")
            }
            Repeater {
                model: Obs.recordings
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    opacity: modelData.kind === "muted" ? 0.6 : 1
                    Kirigami.Icon {
                        Layout.alignment: Qt.AlignTop
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: implicitWidth
                        source: modelData.kind === "ok" ? "checkmark"
                              : modelData.kind === "muted" ? "audio-volume-muted"
                              : "dialog-warning"
                        color: modelData.kind === "ok" ? Kirigami.Theme.positiveTextColor
                             : modelData.kind === "muted" ? Kirigami.Theme.textColor
                             : Kirigami.Theme.neutralTextColor
                    }
                    QQC2.Label {
                        Layout.alignment: Qt.AlignTop
                        text: modelData.source
                        font.weight: Font.DemiBold
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: modelData.text
                    }
                    Accessible.role: Accessible.StaticText
                    Accessible.name: modelData.source + ": " + modelData.text
                }
            }
        }

        // OBS scene -> Rostrum scene
        ColumnLayout {
            Layout.fillWidth: true
            visible: Obs.state !== "notInstalled" && Obs.state !== "neverRun"
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 3
                text: i18nc("@title", "When OBS switches scenes")
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.8
                text: i18n("Pick a Rostrum scene for an OBS scene, and Rostrum loads it as soon as OBS puts that scene on program, without asking.")
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: !Preferences.autoSaveScenes
                wrapMode: Text.WordWrap
                opacity: 0.8
                text: i18n("Scene changes don't save automatically, so fader moves you haven't saved are discarded when OBS switches.")
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: !Obs.background
                type: Kirigami.MessageType.Information
                text: i18n("Rostrum only follows OBS while this page is open.")
                actions: Kirigami.Action {
                    text: i18nc("@action:button", "Follow OBS While It Runs")
                    icon.name: "media-record"
                    onTriggered: Obs.background = true
                }
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: Obs.sceneMap.length === 0
                wrapMode: Text.WordWrap
                opacity: 0.7
                text: i18n("Start OBS to see its scenes.")
            }
            Repeater {
                model: Obs.sceneMap
                RowLayout {
                    id: mapRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.largeSpacing

                    RowLayout {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                        spacing: Kirigami.Units.smallSpacing
                        opacity: mapRow.modelData.present ? 1 : 0.6
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: mapRow.modelData.present ? mapRow.modelData.obsScene
                                                           : i18nc("@info OBS scene that no longer exists", "%1 (not in OBS)", mapRow.modelData.obsScene)
                            elide: Text.ElideRight
                            font.weight: Font.DemiBold
                        }
                        QQC2.Label {
                            visible: mapRow.modelData.onProgram
                            text: i18nc("@info the OBS scene viewers see now", "On program")
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.positiveTextColor
                        }
                    }
                    QQC2.ComboBox {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                        model: [i18nc("@item:inlistbox scene mapping", "No change")].concat(App.sceneNames)
                        currentIndex: mapRow.modelData.rostrumScene === "" ? 0 : App.sceneNames.indexOf(mapRow.modelData.rostrumScene) + 1
                        onActivated: index => Obs.setSceneMapping(mapRow.modelData.obsScene, index === 0 ? "" : App.sceneNames[index - 1])
                        Accessible.name: i18nc("@label accessible", "Rostrum scene for the OBS scene %1", mapRow.modelData.obsScene)
                    }
                    QQC2.ToolButton {
                        visible: !mapRow.modelData.present
                        icon.name: "edit-delete"
                        text: i18nc("@action:button", "Forget")
                        display: QQC2.AbstractButton.IconOnly
                        onClicked: Obs.setSceneMapping(mapRow.modelData.obsScene, "")
                        Accessible.name: i18nc("@action:button accessible", "Forget the mapping for %1", mapRow.modelData.obsScene)
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }
            }
        }

        // The manual route
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            QQC2.ToolButton {
                icon.name: page.manualOpen ? "arrow-down" : "arrow-right"
                text: i18nc("@action:button", "Set it up by hand")
                onClicked: page.manualOpen = !page.manualOpen
                Accessible.description: page.manualOpen ? i18n("Expanded") : i18n("Collapsed")
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: page.manualOpen
                spacing: Kirigami.Units.largeSpacing * 2

                Step {
                    number: 1
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("In OBS, add an <b>Audio Output Capture</b> source (or <b>Audio Capture (PipeWire)</b>) to every scene, and pick:")
                        textFormat: Text.StyledText
                    }
                    NodeName {
                        label: "Rostrum Stream Mix"
                        nodeName: "rostrum.stream"
                        present: page.streamNode !== null
                    }
                }

                Step {
                    number: 2
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("In Settings ▸ Audio, set <b>Mic/Aux</b> to this device, or add an <b>Audio Input Capture</b> with it:")
                        textFormat: Text.StyledText
                    }
                    NodeName {
                        label: "Rostrum Mic"
                        nodeName: "rostrum.mic"
                        present: page.micNode !== null
                    }
                }

                Step {
                    number: 3
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18n("Mute every other audio source that records your mic, your headphones (often called “Default”) or a single app. They skip Rostrum and double the audio. In Settings ▸ Audio, leave Desktop Audio disabled or on Rostrum Stream Mix. Never pick a single bus such as Rostrum Desktop or Rostrum Game: the Stream Mix already has it.")
                    }
                }

                Step {
                    number: 4
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18nc("@info manual setup step", "For separate Twitch VOD audio, add an <b>Audio Output Capture</b> source named <b>Rostrum VOD Mix</b> to every scene, and pick:")
                        textFormat: Text.StyledText
                    }
                    NodeName {
                        label: "Rostrum VOD Mix"
                        nodeName: "rostrum.vod"
                        present: page.vodNode !== null
                    }
                }

                Step {
                    number: 5
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: i18nc("@info manual setup step", "In OBS, open <b>Advanced Audio Properties</b>. Send <b>Rostrum Mic</b> to tracks 1 and 2, <b>Rostrum Stream Mix</b> to track 1 only, and <b>Rostrum VOD Mix</b> to track 2 only. Keep Desktop Audio off both tracks. In <b>Settings ▸ Output</b>, enable <b>Twitch VOD Track</b> and set it to 2.")
                        textFormat: Text.StyledText
                    }
                }
            }
        }

        Item {
            implicitHeight: Kirigami.Units.largeSpacing
        }
    }

    ObsSetupDialog {
        id: previewDialog
    }
}
