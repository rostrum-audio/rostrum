import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kquickcontrols as KQuickControls
import Rostrum

// Plasma-style settings groups: General, Hotkeys, Mixer, Apps, Advanced, About.
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
                text: i18nc("@option:check", "Confirm before switching scenes")
                description: i18n("Ask before a scene switch discards fader moves you have not saved.")
                checked: Preferences.confirmSceneSwitch
                onToggled: Preferences.confirmSceneSwitch = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Scroll to adjust faders")
                description: i18n("Turn off if you scroll the page and move faders by accident.")
                checked: Preferences.scrollToAdjust
                onToggled: Preferences.scrollToAdjust = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Quit Rostrum")
                description: Desktop.trayAvailable ? i18n("Closing the window keeps Rostrum in the tray. After quitting, audio keeps flowing through the mix, but hotkeys and the tray stop.")
                                                   : i18n("Audio keeps flowing through the mix, but hotkeys stop.")
                icon.name: "application-exit"
                onClicked: App.quit()
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
            Repeater {
                model: Preferences.hotkeys
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
