import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// Noise removal, EQ and dynamics that PipeWire runs on the mic, for the stream mic and for the
// apps that record it. Each module has a switch and its main sliders; the rest sit behind "More".
QQC2.ScrollView {
    id: page

    contentWidth: availableWidth
    QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

    readonly property var presetIds: MicFilters.presets.map(p => p.id)

    Kirigami.PlaceholderMessage {
        parent: page
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: !MicFilters.available
        icon.name: "audio-input-microphone"
        text: i18nc("@info:placeholder", "Mic filters are not available")
        explanation: MicFilters.unavailableReason
    }

    ColumnLayout {
        visible: MicFilters.available
        width: page.availableWidth
        spacing: 0

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.bottomMargin: 0
            visible: MicFilters.error !== ""
            type: MicFilters.state === "failed" ? Kirigami.MessageType.Error : Kirigami.MessageType.Warning
            text: MicFilters.error
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.bottomMargin: 0
            visible: MicFilters.gainWarning
            type: Kirigami.MessageType.Warning
            text: i18n("Mic gain is above 100 %, so the limiter works harder than it should. Turn the gain down on the mixer and raise the compressor's makeup gain instead.")
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Mic Filters")
        }
        FormCard.FormCard {
            FormCard.FormSwitchDelegate {
                text: i18nc("@option:check", "Clean up my mic")
                description: {
                    if (!MicFilters.enabled) {
                        return i18n("Noise removal, EQ and compression for your stream mic and the apps you talk in. Your mic itself is never changed.")
                    }
                    switch (MicFilters.state) {
                    case "starting":
                        return i18n("Starting…")
                    case "active":
                        return i18n("Running inside PipeWire, so they keep working while Rostrum is closed.")
                    default:
                        return i18n("Your mic is used without filters.")
                    }
                }
                checked: MicFilters.enabled
                onToggled: MicFilters.enabled = checked
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                id: presetBox
                readonly property bool custom: MicFilters.preset === "custom"
                text: i18nc("@label:listbox", "Preset")
                description: custom ? i18n("Your own settings. Picking a preset replaces them.")
                                    : MicFilters.presets[currentIndex]?.description ?? ""
                model: MicFilters.presets.map(p => p.label).concat(custom ? [i18nc("@item:inlistbox mic filter preset", "Custom")] : [])
                currentIndex: custom ? MicFilters.presets.length : Math.max(0, page.presetIds.indexOf(MicFilters.preset))
                onActivated: index => {
                    if (index < MicFilters.presets.length) {
                        MicFilters.applyPreset(page.presetIds[index])
                    }
                }
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormComboBoxDelegate {
                readonly property var scopes: ["all", "stream"]
                text: i18nc("@label:listbox", "Filter the mic for")
                description: MicFilters.scope === "all"
                             ? i18n("Discord, browsers and other apps that record your mic get the filtered mic. Audio tools and recorders keep your plain mic unless you choose otherwise below.")
                             : i18n("Only your stream mic is filtered. Apps record your plain mic unless you choose otherwise below.")
                model: [i18nc("@item:inlistbox mic filter scope", "The stream and every app"),
                        i18nc("@item:inlistbox mic filter scope", "Only the stream")]
                currentIndex: Math.max(0, scopes.indexOf(MicFilters.scope))
                onActivated: index => MicFilters.scope = scopes[index]
            }
        }

        MicCheckCard {}

        Repeater {
            model: MicFilters.modules.filter(m => !m.needsDenoise || MicFilters.hasDenoise)
            delegate: ColumnLayout {
                id: moduleCard
                required property var modelData
                readonly property string moduleId: modelData.id
                readonly property bool on: MicFilters.revision, MicFilters.isOn(moduleId)
                readonly property var mainParams: modelData.params.filter(p => !p.advanced)
                readonly property var moreParams: modelData.params.filter(p => p.advanced)
                property bool showMore: false
                Layout.fillWidth: true
                spacing: 0

                FormCard.FormHeader {
                    title: moduleCard.modelData.label
                }
                FormCard.FormCard {
                    FormCard.FormSwitchDelegate {
                        text: i18nc("@option:check mic filter module", "On")
                        description: moduleCard.modelData.description
                        checked: moduleCard.on
                        onToggled: MicFilters.setOn(moduleCard.moduleId, checked)
                        Accessible.name: moduleCard.modelData.label
                    }
                    Repeater {
                        model: moduleCard.mainParams
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 0
                            FormCard.FormDelegateSeparator {}
                            FilterSlider {
                                moduleId: moduleCard.moduleId
                                param: parent.modelData
                                enabled: moduleCard.on
                            }
                        }
                    }
                    FormCard.FormDelegateSeparator {
                        visible: moduleCard.moreParams.length > 0 || moduleCard.modelData.switches.length > 0
                    }
                    FormCard.FormButtonDelegate {
                        visible: moduleCard.moreParams.length > 0 || moduleCard.modelData.switches.length > 0
                        text: moduleCard.showMore ? i18nc("@action:button", "Fewer settings") : i18nc("@action:button", "More settings")
                        icon.name: moduleCard.showMore ? "arrow-up" : "arrow-down"
                        onClicked: moduleCard.showMore = !moduleCard.showMore
                        Accessible.description: moduleCard.showMore ? i18n("Expanded") : i18n("Collapsed")
                    }
                    Repeater {
                        model: moduleCard.showMore ? moduleCard.modelData.switches : []
                        delegate: ColumnLayout {
                            id: switchRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 0
                            FormCard.FormDelegateSeparator {}
                            FormCard.FormSwitchDelegate {
                                text: switchRow.modelData.label
                                description: switchRow.modelData.description
                                enabled: moduleCard.on
                                checked: MicFilters.revision, MicFilters.isOn(moduleCard.moduleId, switchRow.modelData.key)
                                onToggled: MicFilters.setOn(moduleCard.moduleId, checked, switchRow.modelData.key)
                            }
                        }
                    }
                    Repeater {
                        model: moduleCard.showMore ? moduleCard.moreParams : []
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 0
                            FormCard.FormDelegateSeparator {}
                            FilterSlider {
                                moduleId: moduleCard.moduleId
                                param: parent.modelData
                                enabled: moduleCard.on
                            }
                        }
                    }
                    FormCard.FormDelegateSeparator {
                        visible: moduleCard.showMore
                    }
                    FormCard.FormButtonDelegate {
                        visible: moduleCard.showMore
                        text: i18nc("@action:button", "Reset %1", moduleCard.modelData.label)
                        description: i18n("Back to the Streaming preset's settings for this filter.")
                        icon.name: "edit-undo"
                        onClicked: MicFilters.resetModule(moduleCard.moduleId)
                    }
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Apps")
        }
        FormCard.FormCard {
            FormCard.FormTextDelegate {
                visible: MicFilters.apps.length === 0
                text: i18n("Apps that record your mic show up here while they run.")
                textItem.wrapMode: Text.WordWrap
            }
            Repeater {
                model: MicFilters.apps
                delegate: ColumnLayout {
                    id: appRow
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 0
                    FormCard.FormDelegateSeparator {
                        visible: appRow.index > 0
                    }
                    FormCard.FormSwitchDelegate {
                        text: appRow.modelData.name
                        icon.name: appRow.modelData.icon !== "" ? appRow.modelData.icon : "application-x-executable"
                        description: {
                            const m = appRow.modelData
                            let state = !m.running ? i18n("Not running. Your choice is kept for next time.")
                                      : m.filtered ? i18n("Hears the filtered mic.")
                                      : (m.recordsFrom ?? "") !== "" ? i18nc("@info %1 is a device, e.g. Easy Effects Source",
                                                                             "Easy Effects moved it to %1.", m.recordsFrom)
                                                   : i18n("Hears your plain mic.")
                            if (m.running && m.choice === "default" && m.excludedByDefault) {
                                state += " " + i18n("An audio tool, so it gets the plain mic unless you turn this on.")
                            } else if (m.choice !== "default") {
                                state += " " + i18n("Your choice.")
                            }
                            return state
                        }
                        checked: appRow.modelData.wantsFiltered
                        onToggled: MicFilters.setAppFiltered(appRow.modelData.key, checked)
                        Accessible.name: i18nc("@option:check accessible", "Filtered mic for %1", appRow.modelData.name)
                    }
                }
            }
        }
        FormCard.FormSectionText {
            text: i18n("Apps can also pick “Rostrum Filtered Mic” or your own mic in their settings. OBS is set up on the OBS page.")
        }

        Item {
            Layout.preferredHeight: Kirigami.Units.largeSpacing
        }
    }

    component FilterSlider: FormCard.AbstractFormDelegate {
        id: row
        required property string moduleId
        required property var param
        readonly property double current: MicFilters.revision, MicFilters.value(moduleId, param.key)
        Layout.fillWidth: true
        background: null
        focusPolicy: Qt.NoFocus
        contentItem: ColumnLayout {
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                QQC2.Label {
                    text: row.param.label
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                QQC2.Label {
                    text: MicFilters.format(row.moduleId, row.param.key, slider.value)
                    font.features: { "tnum": 1 }
                    opacity: 0.8
                }
            }
            QQC2.Slider {
                id: slider
                Layout.fillWidth: true
                from: row.param.min
                to: row.param.max
                stepSize: row.param.step
                snapMode: QQC2.Slider.SnapAlways
                value: row.current
                onMoved: MicFilters.setValue(row.moduleId, row.param.key, value)
                Accessible.name: row.param.label
                Accessible.description: MicFilters.format(row.moduleId, row.param.key, value)
            }
            QQC2.Label {
                visible: text !== ""
                text: row.param.description
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                opacity: 0.7
            }
        }
    }
}
