import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// First run: welcome, devices, apps, startup, privacy and updates, OBS, then a summary that creates
// the mix. Every choice applies as soon as it is made, so Skip keeps them and uses defaults for the
// rest. "Create Mix" stays on the last step and shows the PipeWire error if node creation fails.
QQC2.Pane {
    id: wizard

    property int step: 0
    property bool creating: false
    // Set Up OBS was pressed before the mix existed: create it, then show the preview.
    property bool obsPreparing: false
    readonly property var steps: [
        { id: "welcome", title: i18nc("@title wizard step", "Welcome"),
          heading: i18nc("@title", "Welcome to Rostrum"),
          lead: i18n("Your game, voice chat, music and mic each get their own fader, and you decide what you hear and what your stream hears. Setup takes about a minute.") },
        { id: "headphones", title: i18nc("@title wizard step", "Headphones"),
          heading: i18nc("@title", "Where do you listen?"),
          lead: i18n("Pick your headphones. Press Test to hear a short chime in them.") },
        { id: "mic", title: i18nc("@title wizard step", "Mic"),
          heading: i18nc("@title", "Which mic is yours?"),
          lead: i18n("Say something. The meter under your mic moves.") },
        { id: "apps", title: i18nc("@title wizard step", "Apps and Buses"),
          heading: i18nc("@title", "Apps and buses"),
          lead: i18n("Each bus has its own fader and goes to your headphones, your stream, or both. Rostrum can put each app on the right bus by itself.") },
        { id: "startup", title: i18nc("@title wizard step", "Startup"),
          heading: i18nc("@title", "When Rostrum starts"),
          lead: i18n("Your mix keeps playing while Rostrum is closed, but apps find their bus and hotkeys work only while it runs.") },
        CrashReports.available
            ? { id: "privacy", title: i18nc("@title wizard step", "Privacy and Updates"),
                heading: i18nc("@title", "Privacy and updates"),
                lead: i18n("Help fix crashes and stay up to date. Nothing personal is ever collected.") }
            : { id: "privacy", title: i18nc("@title wizard step", "Updates"),
                heading: i18nc("@title", "Updates"),
                lead: i18n("Stay up to date. Nothing about you is sent.") },
        { id: "obs", title: i18nc("@title wizard step", "OBS"),
          heading: i18nc("@title", "Recording with OBS"),
          lead: i18n("OBS should record two things from Rostrum: your mic and the stream mix. Rostrum can set that up in one click and undo it later. Skip this if you use another recorder.") },
        { id: "ready", title: i18nc("@title wizard step", "Ready"),
          heading: i18nc("@title", "You're all set"),
          lead: i18n("Here is your setup. Create the mix and the Mixer opens. Everything can be changed later in Settings.") }
    ]
    readonly property string stepId: steps[step].id
    readonly property bool lastStep: step === steps.length - 1

    readonly property bool wantMeters: visible && stepId === "mic"
    onWantMetersChanged: Devices.metersActive = wantMeters

    Binding {
        target: Obs
        property: "wizardActive"
        value: wizard.visible && wizard.stepId === "obs"
    }

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
            if (wizard.obsPreparing && App.mixReady) {
                wizard.obsPreparing = false
                Obs.refresh()
                obsDialog.open()
            } else if (wizard.obsPreparing && App.mixError !== "") {
                wizard.obsPreparing = false
            }
        }
    }

    // OBS needs Rostrum's devices to exist before it can record them.
    function setUpObs() {
        if (App.mixReady) {
            obsDialog.open()
            return
        }
        obsPreparing = true
        App.createMix()
    }

    function obsText() {
        if (Obs.setUp) {
            return i18nc("@info OBS", "Records Rostrum Mic and Rostrum Stream Mix")
        }
        if (Obs.state === "notInstalled") {
            return i18nc("@info OBS", "Not installed")
        }
        return i18nc("@info OBS", "Not set up yet; the OBS page can do it later")
    }

    function createMix() {
        creating = true
        App.createMix()
        if (App.mixReady) {
            creating = false
            App.finishWizard()
        }
    }

    function crashModeText(mode) {
        return mode === "send" ? i18nc("@info crash reports", "Sent automatically")
             : mode === "never" ? i18nc("@info crash reports", "Never sent")
             : i18nc("@info crash reports", "Ask after a crash")
    }

    function updatesText() {
        if (!Updates.canCheck) {
            return i18nc("@info updates", "Through Flatpak")
        }
        if (!Updates.checkEnabled) {
            return i18nc("@info updates", "Not checked")
        }
        return Updates.canInstall && Updates.autoInstall ? i18nc("@info updates", "Checked daily and installed automatically")
                                                         : i18nc("@info updates", "Checked daily")
    }

    focusPolicy: Qt.NoFocus
    padding: 0

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Step rail: where you are, what is done, and a way back to any finished step.
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 13
            Kirigami.Theme.inherit: false
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            color: Kirigami.Theme.backgroundColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.gridUnit
                spacing: 0

                QQC2.Label {
                    Layout.bottomMargin: Kirigami.Units.largeSpacing
                    text: i18nc("@title", "Setup")
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 1
                    opacity: 0.6
                }

                Repeater {
                    model: wizard.steps
                    delegate: QQC2.AbstractButton {
                        id: railItem
                        required property var modelData
                        required property int index
                        readonly property bool done: index < wizard.step
                        readonly property bool current: index === wizard.step
                        readonly property color lineColor: Qt.alpha(Kirigami.Theme.textColor, 0.2)

                        readonly property bool reachable: done && !wizard.creating

                        Layout.fillWidth: true
                        implicitHeight: Kirigami.Units.gridUnit * 2.4
                        hoverEnabled: reachable
                        focusPolicy: reachable ? Qt.TabFocus : Qt.NoFocus
                        onClicked: if (reachable) wizard.step = index
                        Accessible.name: modelData.title
                        Accessible.description: done ? i18nc("@info accessible", "Done") : current ? i18nc("@info accessible", "Current step") : ""

                        background: Rectangle {
                            radius: Kirigami.Units.cornerRadius
                            color: railItem.hovered || railItem.visualFocus ? Qt.alpha(Kirigami.Theme.highlightColor, 0.12) : "transparent"
                        }

                        contentItem: RowLayout {
                            spacing: Kirigami.Units.largeSpacing

                            Item {
                                Layout.fillHeight: true
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 1.4

                                Rectangle {
                                    visible: railItem.index > 0
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: parent.top
                                    anchors.bottom: dot.top
                                    width: 2
                                    color: railItem.done || railItem.current ? Kirigami.Theme.highlightColor : railItem.lineColor
                                }
                                Rectangle {
                                    visible: railItem.index < wizard.steps.length - 1
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: dot.bottom
                                    anchors.bottom: parent.bottom
                                    width: 2
                                    color: railItem.done ? Kirigami.Theme.highlightColor : railItem.lineColor
                                }
                                Rectangle {
                                    id: dot
                                    anchors.centerIn: parent
                                    width: Kirigami.Units.gridUnit * 1.4
                                    height: width
                                    radius: width / 2
                                    color: railItem.done || railItem.current ? Kirigami.Theme.highlightColor : Kirigami.Theme.backgroundColor
                                    border.width: railItem.done || railItem.current ? 0 : 1.5
                                    border.color: railItem.lineColor

                                    Kirigami.Icon {
                                        visible: railItem.done
                                        anchors.centerIn: parent
                                        width: Kirigami.Units.iconSizes.small
                                        height: width
                                        source: "checkmark"
                                        isMask: true
                                        color: Kirigami.Theme.highlightedTextColor
                                    }
                                    QQC2.Label {
                                        visible: !railItem.done
                                        anchors.centerIn: parent
                                        text: railItem.index + 1
                                        font: Kirigami.Theme.smallFont
                                        color: railItem.current ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                                        opacity: railItem.current ? 1 : 0.7
                                    }
                                }
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: railItem.modelData.title
                                elide: Text.ElideRight
                                font.weight: railItem.current ? Font.DemiBold : Font.Normal
                                opacity: railItem.current || railItem.done ? 1 : 0.6
                            }
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    opacity: 0.6
                    text: i18nc("@info", "Rostrum %1", App.version)
                }
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            QQC2.ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

                Item {
                    width: scroll.availableWidth
                    implicitHeight: body.implicitHeight + Kirigami.Units.gridUnit * 3

                    ColumnLayout {
                        id: body
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: Kirigami.Units.gridUnit * 1.5
                        width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 36)
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.Heading {
                            Layout.fillWidth: true
                            level: 1
                            text: wizard.steps[wizard.step].heading
                            wrapMode: Text.WordWrap
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            Layout.bottomMargin: Kirigami.Units.smallSpacing
                            text: wizard.steps[wizard.step].lead
                            wrapMode: Text.WordWrap
                            opacity: 0.8
                        }

                        StackLayout {
                            Layout.fillWidth: true
                            currentIndex: wizard.step
                            Layout.preferredHeight: children[currentIndex] ? children[currentIndex].implicitHeight : 0

                            // 1. Welcome
                            ColumnLayout {
                                spacing: Kirigami.Units.gridUnit
                                MixDiagram {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.topMargin: Kirigami.Units.largeSpacing
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 3
                                    columnSpacing: Kirigami.Units.gridUnit
                                    Repeater {
                                        model: [
                                            { icon: "view-media-equalizer", title: i18nc("@title", "A bus for everything"),
                                              text: i18n("Game, voice, music, alerts and desktop audio on their own faders.") },
                                            { icon: "audio-headphones", title: i18nc("@title", "Two mixes"),
                                              text: i18n("Music for your viewers without it playing in your ears.") },
                                            { icon: "media-record", title: i18nc("@title", "Ready for OBS"),
                                              text: i18n("OBS records one clean stream mix and your mic.") }
                                        ]
                                        delegate: ColumnLayout {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 1
                                            Layout.alignment: Qt.AlignTop
                                            spacing: Kirigami.Units.smallSpacing
                                            Kirigami.Icon {
                                                implicitWidth: Kirigami.Units.iconSizes.medium
                                                implicitHeight: implicitWidth
                                                source: modelData.icon
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                text: modelData.title
                                                font.weight: Font.DemiBold
                                                wrapMode: Text.WordWrap
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                text: modelData.text
                                                wrapMode: Text.WordWrap
                                                opacity: 0.75
                                            }
                                        }
                                    }
                                }
                            }

                            // 2. Headphones
                            DeviceList {
                                groupName: i18nc("@title", "Headphones")
                                devices: Devices.outputs
                                selected: Devices.headphones
                                inUse: Devices.headphonesInUse
                                onPicked: name => Devices.headphones = name
                            }

                            // 3. Mic
                            ColumnLayout {
                                spacing: Kirigami.Units.largeSpacing
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
                                        text: i18n("The mic button in the header does the same, any time. Ctrl+M too.")
                                    }
                                }
                            }

                            // 4. Apps and buses
                            ColumnLayout {
                                spacing: 0
                                FormCard.FormCard {
                                    maximumWidth: width - 2
                                    FormCard.FormSwitchDelegate {
                                        text: i18nc("@option:check", "Put apps on buses automatically")
                                        description: i18n("Discord goes to Voice, Spotify to Music, Steam games to Game. OBS and audio tools are never moved, and your own choices always win.")
                                        checked: Preferences.autoAssign
                                        onToggled: Preferences.autoAssign = checked
                                    }
                                }
                                FormCard.FormHeader {
                                    title: i18nc("@title:group", "Your buses")
                                    maximumWidth: width - 2
                                }
                                FormCard.FormCard {
                                    maximumWidth: width - 2
                                    Repeater {
                                        model: Mixer.buses
                                        delegate: FormCard.AbstractFormDelegate {
                                            id: busRow
                                            required property string name
                                            required property color busColor
                                            required property int destination
                                            required property bool isInput
                                            required property string autoCategory
                                            readonly property var autoLabels: ({
                                                game: i18nc("@info bus receives", "Games"),
                                                voice: i18nc("@info bus receives", "Voice chat"),
                                                music: i18nc("@info bus receives", "Music players"),
                                                alerts: i18nc("@info bus receives", "Stream alerts"),
                                                desktop: i18nc("@info bus receives", "Everything else")
                                            })
                                            Layout.fillWidth: true
                                            background: null
                                            focusPolicy: Qt.NoFocus
                                            contentItem: RowLayout {
                                                spacing: Kirigami.Units.largeSpacing
                                                Rectangle {
                                                    implicitWidth: 4
                                                    implicitHeight: Kirigami.Units.gridUnit * 1.6
                                                    radius: 2
                                                    color: busRow.busColor
                                                }
                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 0
                                                    QQC2.Label {
                                                        text: busRow.name
                                                        font.weight: Font.DemiBold
                                                    }
                                                    QQC2.Label {
                                                        Layout.fillWidth: true
                                                        font: Kirigami.Theme.smallFont
                                                        opacity: 0.7
                                                        elide: Text.ElideRight
                                                        text: busRow.isInput ? i18nc("@info", "Your mic")
                                                            : (Preferences.autoAssign && busRow.autoLabels[busRow.autoCategory])
                                                              ? i18nc("@info %1 is a kind of app", "Receives %1", busRow.autoLabels[busRow.autoCategory])
                                                              : i18nc("@info", "Apps you drag here")
                                                    }
                                                }
                                                QQC2.Label {
                                                    text: busRow.destination === 0 ? i18nc("destination", "Headphones")
                                                        : busRow.destination === 1 ? i18nc("destination", "Stream")
                                                        : i18nc("destination", "Headphones and stream")
                                                    opacity: 0.7
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // 5. Startup
                            ColumnLayout {
                                spacing: 0
                                FormCard.FormCard {
                                    maximumWidth: width - 2
                                    FormCard.FormSwitchDelegate {
                                        text: i18nc("@option:check", "Launch Rostrum at login")
                                        description: i18n("Recommended. The mix is ready before Discord or OBS start, and apps land on their bus from the first second.")
                                        checked: Desktop.launchAtLogin
                                        onToggled: Desktop.launchAtLogin = checked
                                    }
                                    FormCard.FormDelegateSeparator {}
                                    FormCard.FormSwitchDelegate {
                                        text: i18nc("@option:check", "Start hidden in the tray")
                                        description: Desktop.trayAvailable ? i18n("At login, Rostrum waits in the tray instead of opening its window.")
                                                                           : i18n("Needs a system tray, and this desktop does not show one.")
                                        enabled: Desktop.launchAtLogin && Desktop.trayAvailable
                                        checked: Desktop.startInTray
                                        onToggled: Desktop.startInTray = checked
                                    }
                                }
                                FormCard.FormHeader {
                                    title: i18nc("@title:group", "Hotkeys")
                                    maximumWidth: width - 2
                                }
                                FormCard.FormCard {
                                    maximumWidth: width - 2
                                    Repeater {
                                        model: Preferences.hotkeys.slice(0, 3)
                                        delegate: FormCard.FormTextDelegate {
                                            required property var modelData
                                            text: modelData.label
                                            trailing: QQC2.Label {
                                                text: modelData.shortcut.length > 0 ? modelData.shortcut : i18nc("@info no shortcut", "None")
                                                font.family: "monospace"
                                                opacity: 0.8
                                            }
                                        }
                                    }
                                    FormCard.FormTextDelegate {
                                        text: i18n("These work in any app while Rostrum runs. Change them in Settings → Hotkeys.")
                                        textItem.wrapMode: Text.WordWrap
                                        textItem.font: Kirigami.Theme.smallFont
                                        textItem.opacity: 0.7
                                    }
                                }
                            }

                            // 6. Privacy and updates
                            PrivacyChoices {
                                onExampleRequested: exampleDialog.openExample()
                            }

                            // 7. OBS
                            ColumnLayout {
                                spacing: Kirigami.Units.largeSpacing
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.largeSpacing
                                    Item {
                                        Layout.alignment: Qt.AlignTop
                                        implicitWidth: Kirigami.Units.iconSizes.medium
                                        implicitHeight: implicitWidth
                                        QQC2.BusyIndicator {
                                            anchors.fill: parent
                                            running: Obs.state === "connecting" || Obs.busy || wizard.obsPreparing
                                            visible: running
                                            Accessible.name: i18n("Checking OBS")
                                        }
                                        Kirigami.Icon {
                                            anchors.fill: parent
                                            visible: !(Obs.state === "connecting" || Obs.busy || wizard.obsPreparing)
                                            source: Obs.setUp ? "checkmark" : Obs.state === "notInstalled" ? "help-about" : "media-record"
                                            color: Obs.setUp ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.textColor
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
                                            visible: !Obs.setUp && Obs.state !== "notInstalled" && Obs.state !== "neverRun"
                                            QQC2.Button {
                                                highlighted: true
                                                icon.name: "configure"
                                                enabled: !wizard.obsPreparing && !App.mixBusy && (!App.mixReady || Obs.canApply)
                                                text: Obs.onlyDoubling ? i18nc("@action:button", "Fix OBS…") : i18nc("@action:button", "Set Up OBS…")
                                                onClicked: wizard.setUpObs()
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                visible: !App.mixReady
                                                wrapMode: Text.WordWrap
                                                opacity: 0.7
                                                text: i18n("Rostrum creates its devices first, so OBS can find them. You see every change before it is made.")
                                            }
                                        }
                                    }
                                }
                                FormCard.FormHeader {
                                    title: i18nc("@title:group", "While you stream")
                                    maximumWidth: width - 2
                                }
                                ObsChoices {
                                    Layout.fillWidth: true
                                }
                            }

                            // 8. Ready
                            ColumnLayout {
                                spacing: Kirigami.Units.largeSpacing
                                FormCard.FormCard {
                                    maximumWidth: width - 2
                                    Repeater {
                                        model: [
                                            { icon: "audio-headphones", label: i18nc("@label", "Headphones"), value: App.headphonesText, step: 1 },
                                            { icon: "audio-input-microphone", label: i18nc("@label", "Mic"), value: App.micText, step: 2 },
                                            { icon: "applications-multimedia", label: i18nc("@label", "Apps"),
                                              value: Preferences.autoAssign ? i18nc("@info", "Put on buses automatically") : i18nc("@info", "You put them on buses"), step: 3 },
                                            { icon: "system-run", label: i18nc("@label", "Startup"),
                                              value: !Desktop.launchAtLogin ? i18nc("@info", "When you open it")
                                                     : Desktop.startInTray && Desktop.trayAvailable ? i18nc("@info", "At login, in the tray")
                                                     : i18nc("@info", "At login"), step: 4 },
                                            { icon: "security-high", label: i18nc("@label", "Crash reports"), value: wizard.crashModeText(CrashReports.mode), step: 5 },
                                            { icon: "update-none", label: i18nc("@label", "Updates"), value: wizard.updatesText(), step: 5 },
                                            { icon: "media-record", label: i18nc("@label", "OBS"), value: wizard.obsText(), step: 6 }
                                        ].filter(row => row.icon !== "security-high" || CrashReports.available)
                                        delegate: FormCard.FormButtonDelegate {
                                            required property var modelData
                                            icon.name: modelData.icon
                                            text: modelData.label
                                            description: modelData.value
                                            enabled: !wizard.creating
                                            onClicked: wizard.step = modelData.step
                                            Accessible.description: i18nc("@info accessible", "Change")
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
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                QQC2.Button {
                    visible: !wizard.lastStep
                    flat: true
                    text: i18nc("@action:button", "Skip Setup")
                    onClicked: App.skipWizard()
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Keep the choices made so far, use the defaults for the rest, and go to the Mixer")
                }
                Item {
                    Layout.fillWidth: true
                }
                QQC2.Label {
                    text: i18nc("@info wizard progress", "Step %1 of %2", wizard.step + 1, wizard.steps.length)
                    font: Kirigami.Theme.smallFont
                    opacity: 0.6
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                }
                QQC2.BusyIndicator {
                    visible: wizard.creating
                    running: visible
                    implicitHeight: createButton.implicitHeight
                    implicitWidth: implicitHeight
                    Accessible.name: i18n("Creating the mix")
                }
                QQC2.Button {
                    visible: wizard.step > 0
                    enabled: !wizard.creating
                    text: i18nc("@action:button", "Back")
                    icon.name: "go-previous"
                    onClicked: wizard.step -= 1
                }
                QQC2.Button {
                    id: nextButton
                    readonly property bool skipsObs: wizard.stepId === "obs" && !Obs.setUp
                    visible: !wizard.lastStep
                    highlighted: !skipsObs
                    text: wizard.step === 0 ? i18nc("@action:button", "Get Started")
                                            : skipsObs ? i18nc("@action:button skip the OBS setup step", "Skip")
                                                       : i18nc("@action:button", "Next")
                    icon.name: "go-next"
                    onClicked: wizard.step += 1
                }
                QQC2.Button {
                    id: createButton
                    visible: wizard.lastStep
                    highlighted: true
                    enabled: !wizard.creating
                    text: wizard.creating ? i18nc("@action:button", "Creating…") : i18nc("@action:button", "Create Mix")
                    icon.name: "dialog-ok-apply"
                    onClicked: wizard.createMix()
                }
            }
        }
    }

    CrashReportDialog {
        id: exampleDialog
    }
    ObsSetupDialog {
        id: obsDialog
    }

    onStepChanged: {
        scroll.contentItem.contentY = 0
        nextButton.visible ? nextButton.forceActiveFocus() : createButton.forceActiveFocus()
    }
    Component.onCompleted: nextButton.forceActiveFocus()
}
