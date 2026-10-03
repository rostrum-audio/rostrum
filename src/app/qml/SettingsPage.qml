import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtCore
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kquickcontrols as KQuickControls
import Rostrum

// Plasma-style settings groups: General, Hotkeys, Mixer, Ducking, Apps, OBS, Privacy, Updates,
// Advanced, About.
QQC2.ScrollView {
    id: page

    contentWidth: availableWidth
    QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

    ColumnLayout {
        width: page.availableWidth
        spacing: 0

        FormCard.FormHeader {
            title: i18nc("@title:group", "General")
        }
        FormCard.FormCard {
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Launch at login")
                description: i18n("Start Rostrum when you log in, so the mix is ready before Discord or OBS.")
                checked: Desktop.launchAtLogin
                onToggled: Desktop.launchAtLogin = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Start in tray")
                description: Desktop.trayAvailable ? i18n("At login, start with the window hidden. Click the tray icon to show it.")
                                                   : i18n("Needs a system tray, and this desktop does not show one.")
                enabled: Desktop.launchAtLogin && Desktop.trayAvailable
                checked: Desktop.startInTray
                onToggled: Desktop.startInTray = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Hide to tray when closed")
                description: !Desktop.trayAvailable ? i18n("Needs a system tray. Without one, closing the window quits Rostrum.")
                           : checked ? i18n("Closing the window keeps Rostrum running in the tray, with hotkeys working.")
                                     : i18n("Closing the window quits Rostrum. Audio keeps flowing through the mix, but hotkeys stop.")
                enabled: Desktop.trayAvailable
                checked: Desktop.closeToTray
                onToggled: Desktop.closeToTray = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Hide to tray when minimized")
                description: Desktop.trayAvailable ? i18n("Minimizing the window hides it from the task bar. Click the tray icon to show it.")
                                                   : i18n("Needs a system tray, and this desktop does not show one.")
                enabled: Desktop.trayAvailable
                checked: Desktop.minimizeToTray
                onToggled: Desktop.minimizeToTray = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Save scene changes automatically")
                description: i18n("Fader, mute and destination changes save to the live scene a moment after you make them. Turn off to keep scenes fixed until you press Save.")
                checked: Preferences.autoSaveScenes
                onToggled: Preferences.autoSaveScenes = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Confirm before switching scenes")
                description: Preferences.autoSaveScenes ? i18n("Not needed while scene changes save automatically.")
                                                        : i18n("Ask before a scene switch discards fader moves you have not saved.")
                enabled: !Preferences.autoSaveScenes
                checked: Preferences.confirmSceneSwitch
                onToggled: Preferences.confirmSceneSwitch = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "Scene fade")
                description: i18n("Bus and master levels glide to the new scene instead of jumping. The mic always switches at once.")
                readonly property var lengths: [0, 150, 300, 600, 1000]
                model: [i18nc("@item:inlistbox scene fade", "Off (switch at once)"),
                        i18nc("@item:inlistbox scene fade", "150 ms"),
                        i18nc("@item:inlistbox scene fade", "300 ms"),
                        i18nc("@item:inlistbox scene fade", "600 ms"),
                        i18nc("@item:inlistbox scene fade", "1 second")]
                currentIndex: Math.max(0, lengths.indexOf(Preferences.sceneFadeMs))
                onActivated: index => Preferences.sceneFadeMs = lengths[index]
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Scroll to adjust faders")
                description: i18n("Turn off if you scroll the page and move faders by accident.")
                checked: Preferences.scrollToAdjust
                onToggled: Preferences.scrollToAdjust = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Show hotkey changes on screen")
                description: i18n("When a hotkey or another app mutes the mic, sets off panic mute or switches scenes while Rostrum's window is hidden or in the background, show it briefly on screen.")
                checked: Preferences.osdFeedback
                onToggled: Preferences.osdFeedback = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Quit Rostrum")
                description: Desktop.trayAvailable ? i18n("After quitting, audio keeps flowing through the mix, but hotkeys and the tray stop.")
                                                   : i18n("Audio keeps flowing through the mix, but hotkeys stop.")
                icon.name: "application-exit"
                onClicked: App.quit()
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Restart Rostrum")
                description: i18n("Quits and opens Rostrum again, for example after installing a new version. Audio keeps flowing meanwhile.")
                icon.name: "view-refresh"
                onClicked: App.restart(false)
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Hotkeys")
        }
        FormCard.FormCard {
            FormCard.FormTextDelegate {
                text: i18n("Click a shortcut, then press the keys. Backspace or the clear button removes it.")
                description: Desktop.shortcutBackend === "kglobalaccel"
                             ? i18n("They work in any app and also appear in System Settings → Shortcuts.")
                             : Desktop.shortcutBackend === "portal"
                               ? i18n("They work in any app once the desktop allows them. It may ask you to confirm.")
                               : ""
                textItem.wrapMode: Text.WordWrap
            }
            FormCard.FormTextDelegate {
                text: i18n("Push to talk and push to mute act while the keys are held. Where the desktop does not report the keys going up, and in the window, each press turns them on or off instead.")
                textItem.wrapMode: Text.WordWrap
                textItem.font: Kirigami.Theme.smallFont
                textItem.opacity: 0.7
            }
        }

        Repeater {
            model: [
                { group: "mic", title: i18nc("@title:group hotkeys", "Mic") },
                { group: "stream", title: i18nc("@title:group hotkeys", "Stream and Headphones") },
                { group: "scenes", title: i18nc("@title:group hotkeys", "Scenes") },
                { group: "buses", title: i18nc("@title:group hotkeys", "Buses") }
            ]
            delegate: ColumnLayout {
                id: hotkeyGroup
                required property var modelData
                readonly property var rows: Preferences.hotkeys.filter(row => row.group === modelData.group)
                visible: rows.length > 0
                Layout.fillWidth: true
                spacing: 0

                FormCard.FormHeader {
                    title: hotkeyGroup.modelData.title
                }
                FormCard.FormCard {
                    Repeater {
                        model: hotkeyGroup.rows
                        delegate: FormCard.AbstractFormDelegate {
                            id: hotkeyRow
                            required property var modelData
                            readonly property string conflict: Preferences.hotkeyConflict(modelData.id, modelData.shortcut)
                            Layout.fillWidth: true
                            background: null
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    QQC2.Label {
                                        text: hotkeyRow.modelData.label
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                    QQC2.Label {
                                        visible: text !== ""
                                        text: hotkeyRow.modelData.description
                                        font: Kirigami.Theme.smallFont
                                        opacity: 0.7
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }
                                    QQC2.Label {
                                        visible: hotkeyRow.conflict !== ""
                                        text: i18nc("@info", "Also used by “%1”", hotkeyRow.conflict)
                                        color: Kirigami.Theme.neutralTextColor
                                        font: Kirigami.Theme.smallFont
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }
                                    QQC2.Label {
                                        visible: hotkeyRow.conflict === "" && hotkeyRow.modelData.problem !== ""
                                        text: hotkeyRow.modelData.problem
                                        color: Kirigami.Theme.neutralTextColor
                                        font: Kirigami.Theme.smallFont
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }
                                }
                                KQuickControls.KeySequenceItem {
                                    keySequence: hotkeyRow.modelData.shortcut
                                    modifierlessAllowed: false
                                    multiKeyShortcutsAllowed: false
                                    onKeySequenceModified: Preferences.setHotkeySequence(hotkeyRow.modelData.id, keySequence)
                                    Keys.onPressed: event => {
                                        if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier) {
                                            Preferences.setHotkey(hotkeyRow.modelData.id, "")
                                            event.accepted = true
                                        }
                                    }
                                    Accessible.name: i18nc("@label accessible", "Shortcut for %1", hotkeyRow.modelData.label)
                                    Accessible.description: hotkeyRow.modelData.description
                                }
                                QQC2.ToolButton {
                                    icon.name: "edit-undo"
                                    text: i18nc("@action:button", "Reset to default")
                                    display: QQC2.AbstractButton.IconOnly
                                    enabled: hotkeyRow.modelData.shortcut !== hotkeyRow.modelData.defaultShortcut
                                    onClicked: Preferences.resetHotkey(hotkeyRow.modelData.id)
                                    Accessible.name: i18nc("@action:button accessible", "Reset the shortcut for %1", hotkeyRow.modelData.label)
                                    QQC2.ToolTip.visible: hovered
                                    QQC2.ToolTip.text: text
                                }
                            }
                        }
                    }
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Mixer")
        }
        FormCard.FormCard {
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "Meter speed")
                model: [i18nc("@item:inlistbox meter speed", "Normal (25 updates a second)"),
                        i18nc("@item:inlistbox meter speed", "Low (uses less power)")]
                currentIndex: Preferences.lowMeterSpeed ? 1 : 0
                onActivated: index => Preferences.lowMeterSpeed = index === 1
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Show dB readouts")
                description: i18n("Print the level under every fader. Off shows it only on hover or focus.")
                checked: Preferences.showDb
                onToggled: Preferences.showDb = checked
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Ducking")
        }
        FormCard.FormCard {
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Turn music down while someone speaks")
                description: Preferences.duckingTrigger === "voice"
                             ? i18n("Not saved in scenes. Rostrum listens to the voice chat bus while this is on.")
                             : i18n("Not saved in scenes. Rostrum listens to your mic while this is on, so the desktop's mic indicator stays lit whenever your mic is live, even with Rostrum in the tray.")
                checked: Preferences.duckingEnabled
                onToggled: Preferences.duckingEnabled = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "When")
                enabled: Preferences.duckingEnabled
                readonly property var triggers: ["mic", "voice", "either"]
                model: [i18nc("@item:inlistbox ducking trigger", "You speak (mic)"),
                        i18nc("@item:inlistbox ducking trigger", "Someone speaks in voice chat"),
                        i18nc("@item:inlistbox ducking trigger", "Either")]
                currentIndex: Math.max(0, triggers.indexOf(Preferences.duckingTrigger))
                onActivated: index => Preferences.duckingTrigger = triggers[index]
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18nc("@label", "Turn down")
                description: i18n("The bus that receives voice chat is never turned down by voice chat.")
                enabled: Preferences.duckingEnabled
            }
            Repeater {
                model: Mixer.buses
                delegate: FormCard.FormCheckDelegate {
                    required property string busId
                    required property string name
                    required property bool isInput
                    visible: !isInput
                    enabled: Preferences.duckingEnabled
                    text: name
                    checked: Preferences.duckingBuses.indexOf(busId) >= 0
                    onToggled: Preferences.setDuckingBus(busId, checked)
                }
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "By")
                enabled: Preferences.duckingEnabled
                readonly property var amounts: [-6, -9, -12, -18, -24]
                model: amounts.map(db => i18nc("@item:inlistbox ducking amount in decibels", "%1 dB", db))
                currentIndex: Math.max(0, amounts.indexOf(Preferences.duckingAmountDb))
                onActivated: index => Preferences.duckingAmountDb = amounts[index]
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "Turn down over")
                enabled: Preferences.duckingEnabled
                readonly property var lengths: [20, 50, 100, 250, 500]
                model: lengths.map(ms => i18nc("@item:inlistbox ducking attack", "%1 ms", ms))
                currentIndex: Math.max(0, lengths.indexOf(Preferences.duckingAttackMs))
                onActivated: index => Preferences.duckingAttackMs = lengths[index]
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "Come back over")
                description: i18n("Starts half a second after the speaking stops, so music does not pump between words.")
                enabled: Preferences.duckingEnabled
                readonly property var lengths: [250, 500, 800, 1500, 3000]
                model: lengths.map(ms => i18nc("@item:inlistbox ducking release", "%1 ms", ms))
                currentIndex: Math.max(0, lengths.indexOf(Preferences.duckingReleaseMs))
                onActivated: index => Preferences.duckingReleaseMs = lengths[index]
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Apps")
        }
        FormCard.FormCard {
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Assign apps automatically")
                description: i18n("Rostrum recognises games, voice chat, music players and stream alert tools, and puts each on the bus that receives it. Your own assignments and rules always win. Choose what a bus receives from its right-click menu.")
                checked: Preferences.autoAssign
                onToggled: Preferences.autoAssign = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Forget skipped apps")
                description: Preferences.skippedApps > 0
                             ? i18ncp("@info", "One app you took off its bus stays unassigned. Forget it and it is assigned automatically again.",
                                      "%1 apps you took off their bus stay unassigned. Forget them and they are assigned automatically again.",
                                      Preferences.skippedApps)
                             : i18n("Apps you take off their bus stay unassigned. None so far.")
                icon.name: "edit-clear-history"
                enabled: Preferences.skippedApps > 0
                onClicked: Preferences.forgetSkippedApps()
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "OBS")
        }
        ObsChoices {
            maximumWidth: Kirigami.Units.gridUnit * 30
        }

        FormCard.FormHeader {
            visible: CrashReports.available
            title: i18nc("@title:group", "Privacy")
        }
        FormCard.FormCard {
            visible: CrashReports.available
            FormCard.FormComboBoxDelegate {
                text: i18nc("@label:listbox", "Crash reports")
                description: i18n("A crash report shows where in Rostrum's code it crashed, and which versions of Rostrum, Linux, Qt and PipeWire were running. It goes to Sentry, a crash reporting service, and never includes personal information, names, files or logs.")
                readonly property var modes: ["send", "ask", "never"]
                model: [i18nc("@item:inlistbox crash reports", "Send automatically"),
                        i18nc("@item:inlistbox crash reports", "Ask after a crash"),
                        i18nc("@item:inlistbox crash reports", "Never send")]
                currentIndex: Math.max(0, modes.indexOf(CrashReports.mode))
                onActivated: index => CrashReports.mode = modes[index]
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "What a crash report contains")
                description: i18n("See a complete example built from this computer.")
                icon.name: "document-preview"
                onClicked: exampleDialog.openExample()
            }
            FormCard.FormDelegateSeparator {
                visible: CrashReports.pendingCount > 0
            }
            FormCard.FormButtonDelegate {
                visible: CrashReports.pendingCount > 0
                text: i18ncp("@action:button", "Review the unsent crash report", "Review %1 unsent crash reports", CrashReports.pendingCount)
                description: i18n("Most recent crash: %1", CrashReports.lastCrashDate)
                icon.name: "tools-report-bug"
                onClicked: pendingDialog.openPending()
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Updates")
        }
        FormCard.FormCard {
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
            FormCard.FormDelegateSeparator {
                visible: Updates.canCheck
            }
            FormCard.FormButtonDelegate {
                visible: Updates.canCheck
                text: i18nc("@action:button", "Check now")
                icon.name: "view-refresh"
                enabled: Updates.state !== "checking" && Updates.state !== "downloading"
                description: {
                    const last = Updates.lastChecked ? i18nc("@info", "Last checked %1.", Updates.lastChecked) : i18nc("@info", "Not checked yet.")
                    switch (Updates.state) {
                    case "checking": return i18nc("@info", "Checking…")
                    case "upToDate": return i18nc("@info", "Rostrum %1 is the newest version. %2", App.version, last)
                    case "available": return Updates.errorText
                                             ? i18nc("@info", "Rostrum %1 is available, but it could not be installed: %2", Updates.latestVersion, Updates.errorText)
                                             : i18nc("@info", "Rostrum %1 is available. %2", Updates.latestVersion, last)
                    case "downloading": return i18nc("@info", "Downloading Rostrum %1…", Updates.latestVersion)
                    case "ready": return i18nc("@info", "Rostrum %1 is installed and starts the next time you open Rostrum.", Updates.latestVersion)
                    case "error": return i18nc("@info", "Could not check: %1 %2", Updates.errorText, last)
                    default: return i18nc("@info", "You have Rostrum %1. %2", App.version, last)
                    }
                }
                onClicked: Updates.checkNow()
            }
            FormCard.FormTextDelegate {
                visible: !Updates.canCheck
                text: i18n("Flatpak keeps Rostrum up to date through Discover or your software center.")
                textItem.wrapMode: Text.WordWrap
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Advanced")
        }
        FormCard.FormCard {
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Show node ids")
                description: i18n("Show PipeWire node names and ids next to devices and apps.")
                checked: Preferences.showNodeIds
                onToggled: Preferences.showNodeIds = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Open config folder")
                description: Preferences.configFolder
                icon.name: "folder-open"
                onClicked: Preferences.openConfigFolder()
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Back Up Settings…")
                description: i18n("Saves settings, hotkeys, devices and every scene to one file. Crash report and update choices are left out.")
                icon.name: "document-save-as"
                onClicked: {
                    backupDialog.selectedFile = backupDialog.currentFolder + "/" + Preferences.backupFileName()
                    backupDialog.open()
                }
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Restore…")
                description: i18n("Replaces settings and same-named scenes with those from a backup. The current setup is backed up first.")
                icon.name: "document-revert"
                onClicked: restoreFileDialog.open()
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Rebuild virtual devices")
                description: i18n("Removes and recreates Rostrum's PipeWire nodes. Audio drops for a moment.")
                icon.name: "view-refresh"
                enabled: App.connected
                onClicked: rebuildDialog.open()
            }
            FormCard.FormDelegateSeparator {
                visible: App.hasWirePlumber
            }
            FormCard.FormButtonDelegate {
                visible: App.hasWirePlumber
                text: i18nc("@action:button", "Write app rules now")
                description: i18n("Rules from the default scene, so apps land on their bus before Rostrum starts. Written to %1 and %2.",
                                  Preferences.ruleFiles[0], Preferences.ruleFiles[1])
                icon.name: "document-save"
                onClicked: Preferences.exportRulesNow()
            }
            FormCard.FormTextDelegate {
                visible: !App.hasWirePlumber
                text: i18n("App rules are not written because the session manager is not WirePlumber. Routes may not survive a reboot.")
                textItem.wrapMode: Text.WordWrap
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "About")
        }
        FormCard.FormCard {
            Layout.bottomMargin: Kirigami.Units.largeSpacing * 2
            FormCard.FormTextDelegate {
                text: i18nc("@label", "Rostrum")
                description: i18nc("@info version", "Version %1. A stream mix console for Linux.", App.version)
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18nc("@label", "PipeWire")
                description: App.pipewireVersion || i18nc("@info", "Not connected")
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18nc("@label", "WirePlumber")
                description: App.hasWirePlumber ? (App.wireplumberVersion || i18nc("@info", "Running"))
                                                : i18nc("@info", "Not running")
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18nc("@label", "Log file")
                description: Preferences.logFile
            }
            FormCard.FormDelegateSeparator {
                visible: Preferences.readmeUrl.toString() !== ""
            }
            FormCard.FormButtonDelegate {
                visible: Preferences.readmeUrl.toString() !== ""
                text: i18nc("@action:button", "Open the README")
                icon.name: "help-contents"
                onClicked: Qt.openUrlExternally(Preferences.readmeUrl)
            }
        }
    }

    CrashReportDialog {
        id: exampleDialog
    }
    CrashReportDialog {
        id: pendingDialog
    }

    Dialogs.FileDialog {
        id: backupDialog
        title: i18nc("@title:window", "Back Up Settings")
        fileMode: Dialogs.FileDialog.SaveFile
        defaultSuffix: "toml"
        nameFilters: [i18nc("file filter", "Rostrum backups (*.toml)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: Preferences.backUpTo(selectedFile)
    }

    Dialogs.FileDialog {
        id: restoreFileDialog
        title: i18nc("@title:window", "Restore Settings")
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: [i18nc("file filter", "Rostrum backups (*.toml)"), i18nc("file filter", "All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: {
            restoreDialog.file = selectedFile
            restoreDialog.open()
        }
    }

    Kirigami.PromptDialog {
        id: restoreDialog
        property url file
        title: i18nc("@title:dialog", "Restore Settings?")
        subtitle: i18n("Settings, hotkeys and devices are replaced, and scenes with the same name are overwritten (the old versions go to Recently Deleted). Other scenes stay. Your current setup is saved in %1 first.",
                       Preferences.configFolder + "/backups")
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Restore")
                icon.name: "document-revert"
                onTriggered: {
                    restoreDialog.close()
                    Preferences.restoreFrom(restoreDialog.file)
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: restoreDialog.close()
            }
        ]
    }

    Kirigami.PromptDialog {
        id: rebuildDialog
        title: i18nc("@title:dialog", "Rebuild Virtual Devices?")
        subtitle: i18n("Audio through Rostrum stops for a moment while its nodes are recreated. OBS reconnects on its own.")
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Rebuild")
                icon.name: "view-refresh"
                onTriggered: {
                    Preferences.rebuildMix()
                    rebuildDialog.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onTriggered: rebuildDialog.close()
            }
        ]
    }
}
