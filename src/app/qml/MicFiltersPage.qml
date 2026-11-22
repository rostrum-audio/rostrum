import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Studio Voice: on/off, current mic, three sound presets, Check Mic, scope, apps behind a
// disclosure, and Advanced module rows (Penpot). DSP behaviour is unchanged.
QQC2.ScrollView {
    id: page

    contentWidth: availableWidth
    QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

    readonly property var intensityIds: ["light", "streaming", "noisy"]
    readonly property var allPresetIds: MicFilters.presets.map(p => p.id)
    // Cap the column so wide windows stay readable; Check Your Mic matches this width.
    readonly property real columnMaxWidth: Kirigami.Units.gridUnit * 50
    property bool showAdvanced: false
    property bool showApps: false



    // Broadcast/Custom appear as a fourth, selected, read-only segment only while active (main).
    readonly property bool extraSoundActive: MicFilters.preset === "broadcast"
                                             || MicFilters.preset === "custom"
    readonly property var soundIds: {
        const ids = page.intensityIds.slice()
        if (page.extraSoundActive) {
            ids.push(MicFilters.preset)
        }
        return ids
    }

    readonly property string soundDescription: {
        if (MicFilters.preset === "custom") {
            return i18n("Your own settings. Picking a sound replaces them.")
        }
        if (MicFilters.preset === "broadcast") {
            return MicFilters.presets[page.allPresetIds.indexOf("broadcast")]?.description ?? ""
        }
        const idx = page.intensityIds.indexOf(MicFilters.preset)
        const presetIdx = idx >= 0 ? page.allPresetIds.indexOf(page.intensityIds[idx])
                                   : page.allPresetIds.indexOf("streaming")
        return MicFilters.presets[Math.max(0, presetIdx)]?.description ?? ""
    }

    function soundLabel(id) {
        if (id === "custom") {
            return i18nc("@item:inlistbox mic filter preset", "Custom")
        }
        const idx = page.allPresetIds.indexOf(id)
        return MicFilters.presets[idx]?.label ?? id
    }

    readonly property string enableExplanation: {
        if (!MicFilters.enabled) {
            return i18n("Clean, clear voice for your stream and the apps you talk in. Reduces fans, keyboards and room noise. Your hardware mic is never changed.")
        }
        switch (MicFilters.state) {
        case "starting":
            return i18n("Starting…")
        case "active":
            return i18n("Running inside PipeWire, so it keeps working while Rostrum is closed. Adds about 20 ms of delay while noise removal is on.")
        default:
            return i18n("Your mic is used without filters.")
        }
    }

    Kirigami.PlaceholderMessage {
        parent: page
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: !MicFilters.available
        icon.name: "audio-input-microphone"
        text: i18nc("@info:placeholder", "Studio Voice is not available")
        explanation: MicFilters.unavailableReason
    }

    ColumnLayout {
        visible: MicFilters.available
        width: Math.min(page.availableWidth, page.columnMaxWidth + Kirigami.Units.largeSpacing * 2)
        x: Math.max(0, (page.availableWidth - width) / 2)
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: 0
            visible: MicFilters.error !== ""
            type: MicFilters.state === "failed" ? Kirigami.MessageType.Error : Kirigami.MessageType.Warning
            text: MicFilters.error
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: 0
            visible: MicFilters.gainWarning
            type: Kirigami.MessageType.Warning
            text: i18n("Mic gain is above 100 %, so the limiter works harder than it should. Turn the gain down on the mixer and raise the compressor's makeup gain instead.")
        }

        // On/off + one-line explanation
        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: page.columnMaxWidth
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing
                QQC2.Switch {
                    id: enableSwitch
                    checked: MicFilters.enabled
                    onToggled: MicFilters.enabled = checked
                    Accessible.name: i18nc("@option:check", "Studio Voice")
                    Accessible.description: page.enableExplanation
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    QQC2.Label {
                        text: i18nc("@option:check", "Studio Voice")
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                    }
                    QQC2.Label {
                        text: page.enableExplanation
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font: Kirigami.Theme.smallFont
                        opacity: 0.7
                    }
                }
            }

            // Current mic (read-only) + Change → Devices
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                radius: Kirigami.Units.cornerRadius
                color: Kirigami.Theme.backgroundColor
                border.width: 1
                border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
                implicitHeight: micRow.implicitHeight + Kirigami.Units.smallSpacing * 2

                RowLayout {
                    id: micRow
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Icon {
                        source: "audio-input-microphone"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Layout.preferredWidth
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label {
                            text: i18nc("@label", "Mic")
                            font: Kirigami.Theme.smallFont
                            opacity: 0.7
                        }
                        QQC2.Label {
                            text: App.micText
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            Accessible.name: i18nc("@info accessible", "Current mic %1", App.micText)
                        }
                    }
                    QQC2.ToolButton {
                        text: i18nc("@action:button", "Change")
                        display: QQC2.AbstractButton.TextOnly
                        onClicked: applicationWindow().showPage("devices")
                        Accessible.name: i18nc("@action:button accessible", "Change mic")
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                        QQC2.ToolTip.text: i18nc("@info:tooltip", "Choose mic on the Devices page")
                    }
                }
            }

            // Sounds (segmented) + description. Fourth Broadcast/Custom segment only while active.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    text: i18nc("@label", "Sound")
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Accessible.role: Accessible.Grouping
                    Accessible.name: i18nc("@label accessible", "Sound")

                    Repeater {
                        model: page.soundIds
                        QQC2.AbstractButton {
                            id: segment
                            required property var modelData
                            required property int index
                            readonly property string label: page.soundLabel(modelData)
                            readonly property bool selected: MicFilters.preset === modelData
                            readonly property bool readOnlyExtra: modelData === "broadcast"
                                                                  || modelData === "custom"
                            readonly property bool last: index === page.soundIds.length - 1

                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: Kirigami.Units.gridUnit * 2
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            text: label
                            Accessible.role: Accessible.RadioButton
                            Accessible.checkable: true
                            Accessible.checked: selected
                            Accessible.name: label
                            Accessible.description: segment.readOnlyExtra
                                ? i18nc("@info accessible", "Current sound; pick Natural, Studio or Noisy desk to change")
                                : ""
                            onClicked: {
                                if (segment.readOnlyExtra) {
                                    return
                                }
                                MicFilters.applyPreset(modelData)
                            }

                            background: Rectangle {
                                color: segment.selected ? Kirigami.Theme.highlightColor
                                     : segment.down ? Qt.alpha(Kirigami.Theme.textColor, 0.16)
                                     : segment.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.1)
                                     : Qt.alpha(Kirigami.Theme.textColor, 0.05)
                                border.width: segment.visualFocus ? 2 : 1
                                border.color: segment.visualFocus ? Kirigami.Theme.focusColor
                                            : segment.selected ? Kirigami.Theme.highlightColor
                                            : Qt.alpha(Kirigami.Theme.textColor, 0.18)
                                topLeftRadius: segment.index === 0 ? Kirigami.Units.cornerRadius : 0
                                bottomLeftRadius: segment.index === 0 ? Kirigami.Units.cornerRadius : 0
                                topRightRadius: segment.last ? Kirigami.Units.cornerRadius : 0
                                bottomRightRadius: segment.last ? Kirigami.Units.cornerRadius : 0
                            }
                            contentItem: QQC2.Label {
                                text: segment.text
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                color: segment.selected ? Kirigami.Theme.highlightedTextColor
                                                        : Kirigami.Theme.textColor
                            }
                        }
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: page.soundDescription
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Reduces background noise; loud keyboards may still click. Use Check Mic to hear what viewers get.")
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                }
            }
        }

        MicCheckCard {
            Layout.fillWidth: true
            Layout.maximumWidth: page.columnMaxWidth
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.alignment: Qt.AlignHCenter
            fullWidth: true
        }

        // Scope
        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: page.columnMaxWidth
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing
            QQC2.Label {
                text: i18nc("@label:listbox", "Filter the mic for")
                font.weight: Font.DemiBold
            }
            QQC2.ComboBox {
                id: scopeBox
                Layout.fillWidth: true
                readonly property var scopes: ["all", "stream"]
                model: [i18nc("@item:inlistbox mic filter scope", "The stream and every app"),
                        i18nc("@item:inlistbox mic filter scope", "Only the stream")]
                currentIndex: Math.max(0, scopes.indexOf(MicFilters.scope))
                onActivated: index => MicFilters.scope = scopes[index]
                Accessible.name: i18nc("@label:listbox", "Filter the mic for")
                Accessible.description: scopeHint.text
            }
            QQC2.Label {
                id: scopeHint
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                text: MicFilters.scope === "all"
                      ? i18n("Discord, browsers and other apps that record your mic get Studio Voice. Audio tools and recorders keep your plain mic unless you choose otherwise below.")
                      : i18n("Only your stream mic is filtered. Apps record your plain mic unless you choose otherwise below.")
            }
        }

        // Per-app list behind a disclosure (collapsed by default)
        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: page.columnMaxWidth
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            spacing: 0

            QQC2.ToolButton {
                id: appsToggle
                checkable: true
                checked: page.showApps
                text: i18nc("@action:button", "Apps")
                icon.name: checked ? "arrow-down" : "arrow-right"
                onToggled: page.showApps = checked
                Accessible.description: checked ? i18n("Expanded") : i18n("Collapsed")
            }
            ColumnLayout {
                visible: page.showApps
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: MicFilters.apps.length === 0
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    text: i18n("Apps that record your mic show up here while they run.")
                }
                Repeater {
                    model: MicFilters.apps
                    delegate: RowLayout {
                        id: appRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.Icon {
                            source: appRow.modelData.icon !== "" ? appRow.modelData.icon
                                                                : "application-x-executable"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Layout.preferredWidth
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            QQC2.Label {
                                text: appRow.modelData.name
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                font: Kirigami.Theme.smallFont
                                opacity: 0.7
                                text: {
                                    const m = appRow.modelData
                                    let state = !m.running ? i18n("Not running. Your choice is kept for next time.")
                                              : m.filtered ? i18n("Hears Studio Voice.")
                                              : (m.recordsFrom ?? "") !== ""
                                                ? i18nc("@info %1 is a device, e.g. Easy Effects Source",
                                                        "Easy Effects moved it to %1.", m.recordsFrom)
                                                : i18n("Hears your plain mic.")
                                    if (m.running && m.choice === "default" && m.excludedByDefault) {
                                        state += " " + i18n("An audio tool, so it gets the plain mic unless you turn this on.")
                                    } else if (m.choice !== "default") {
                                        state += " " + i18n("Your choice.")
                                    }
                                    return state
                                }
                            }
                        }
                        QQC2.Switch {
                            checked: appRow.modelData.wantsFiltered
                            onToggled: MicFilters.setAppFiltered(appRow.modelData.key, checked)
                            Accessible.name: i18nc("@option:check accessible", "Filtered mic for %1",
                                                   appRow.modelData.name)
                        }
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    text: i18n("Apps can also pick “Rostrum Filtered Mic” or your own mic in their settings. OBS is set up on the OBS page.")
                }
            }
        }

        // Advanced: collapsed by default; each module one dense row
        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: page.columnMaxWidth
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing

            QQC2.ToolButton {
                id: advancedToggle
                checkable: true
                checked: page.showAdvanced
                text: i18nc("@action:button", "Advanced")
                icon.name: checked ? "arrow-down" : "arrow-right"
                onToggled: page.showAdvanced = checked
                Accessible.description: checked
                    ? i18n("Expanded")
                    : i18n("Rumble filter, noise removal, gate, tone, de-esser, compressor, limiter, and Broadcast.")
            }

            ColumnLayout {
                visible: page.showAdvanced
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    QQC2.Label {
                        text: i18nc("@label:listbox", "Character")
                        font.weight: Font.DemiBold
                    }
                    QQC2.ComboBox {
                        Layout.fillWidth: true
                        model: MicFilters.presets.map(p => p.label).concat(
                            MicFilters.preset === "custom"
                            ? [i18nc("@item:inlistbox mic filter preset", "Custom")] : [])
                        currentIndex: {
                            if (MicFilters.preset === "custom") {
                                return MicFilters.presets.length
                            }
                            return Math.max(0, page.allPresetIds.indexOf(MicFilters.preset))
                        }
                        onActivated: index => {
                            if (index < MicFilters.presets.length) {
                                MicFilters.applyPreset(page.allPresetIds[index])
                            }
                        }
                        Accessible.name: i18nc("@label:listbox", "Character")
                        Accessible.description: i18n("Broadcast is a denser radio sound. Natural, Studio and Noisy desk are above.")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font: Kirigami.Theme.smallFont
                        opacity: 0.7
                        text: i18n("Broadcast is a denser radio sound. Natural, Studio and Noisy desk are above.")
                    }
                }

                Repeater {
                    model: MicFilters.modules.filter(m => !m.needsDenoise || MicFilters.hasDenoise)
                    delegate: ColumnLayout {
                        id: moduleCard
                        required property var modelData
                        readonly property string moduleId: modelData.id
                        readonly property bool on: MicFilters.revision, MicFilters.isOn(moduleId)
                        readonly property var mainParams: modelData.params.filter(p => !p.advanced)
                        readonly property var moreParams: modelData.params.filter(p => p.advanced)
                        readonly property var primaryParam: mainParams.length > 0 ? mainParams[0] : null
                        readonly property var extraMainParams: mainParams.length > 1 ? mainParams.slice(1) : []
                        property bool showMore: false
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        // One row: toggle, name, value, main control (Penpot).
                        Rectangle {
                            Layout.fillWidth: true
                            radius: Kirigami.Units.cornerRadius
                            color: Kirigami.Theme.backgroundColor
                            border.width: 1
                            border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
                            implicitHeight: moduleRow.implicitHeight + Kirigami.Units.smallSpacing * 2

                            RowLayout {
                                id: moduleRow
                                anchors.fill: parent
                                anchors.margins: Kirigami.Units.smallSpacing
                                spacing: Kirigami.Units.largeSpacing

                                QQC2.Switch {
                                    checked: moduleCard.on
                                    onToggled: MicFilters.setOn(moduleCard.moduleId, checked)
                                    Accessible.name: moduleCard.modelData.label
                                    Accessible.description: moduleCard.modelData.description
                                    QQC2.ToolTip.visible: hovered && moduleCard.modelData.description
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    QQC2.ToolTip.text: moduleCard.modelData.description
                                }
                                QQC2.Label {
                                    text: moduleCard.modelData.label
                                    font.weight: Font.DemiBold
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                                    Layout.maximumWidth: Kirigami.Units.gridUnit * 10
                                    elide: Text.ElideRight
                                    QQC2.ToolTip.visible: truncated && hovered
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    QQC2.ToolTip.text: moduleCard.modelData.description || text
                                    HoverHandler {}
                                }
                                QQC2.Label {
                                    visible: moduleCard.primaryParam !== null
                                    // revision is listed so the label updates when the slider moves.
                                    text: moduleCard.primaryParam
                                          ? MicFilters.format(moduleCard.moduleId,
                                                              moduleCard.primaryParam.key,
                                                              (MicFilters.revision,
                                                               MicFilters.value(moduleCard.moduleId,
                                                                                moduleCard.primaryParam.key)))
                                          : ""
                                    font.features: { "tnum": 1 }
                                    opacity: 0.8
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                FilterSlider {
                                    visible: moduleCard.primaryParam !== null
                                    Layout.fillWidth: true
                                    moduleId: moduleCard.moduleId
                                    param: moduleCard.primaryParam ?? ({ key: "", label: "", min: 0, max: 1, step: 0.1 })
                                    enabled: moduleCard.on
                                    compact: true
                                }
                            }
                        }

                        QQC2.ToolButton {
                            visible: moduleCard.extraMainParams.length > 0
                                     || moduleCard.moreParams.length > 0
                                     || moduleCard.modelData.switches.length > 0
                            checkable: true
                            checked: moduleCard.showMore
                            text: checked ? i18nc("@action:button", "Fewer settings")
                                          : i18nc("@action:button", "More settings")
                            icon.name: checked ? "arrow-down" : "arrow-right"
                            onToggled: moduleCard.showMore = checked
                            Accessible.description: checked ? i18n("Expanded") : i18n("Collapsed")
                        }

                        ColumnLayout {
                            visible: moduleCard.showMore
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Repeater {
                                model: moduleCard.extraMainParams
                                FilterSlider {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    moduleId: moduleCard.moduleId
                                    param: modelData
                                    enabled: moduleCard.on
                                }
                            }
                            Repeater {
                                model: moduleCard.modelData.switches
                                delegate: QQC2.Switch {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    enabled: moduleCard.on
                                    checked: MicFilters.revision, MicFilters.isOn(moduleCard.moduleId, modelData.key)
                                    onToggled: MicFilters.setOn(moduleCard.moduleId, checked, modelData.key)
                                    Accessible.description: modelData.description
                                    QQC2.ToolTip.visible: hovered && modelData.description
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    QQC2.ToolTip.text: modelData.description
                                }
                            }
                            Repeater {
                                model: moduleCard.moreParams
                                FilterSlider {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    moduleId: moduleCard.moduleId
                                    param: modelData
                                    enabled: moduleCard.on
                                }
                            }
                            QQC2.Button {
                                text: i18nc("@action:button", "Reset %1", moduleCard.modelData.label)
                                icon.name: "edit-undo"
                                onClicked: MicFilters.resetModule(moduleCard.moduleId)
                                Accessible.description: i18n("Back to the Studio sound's settings for this filter.")
                            }
                        }
                    }
                }
            }
        }
    }

    component FilterSlider: ColumnLayout {
        id: row
        required property string moduleId
        required property var param
        property bool enabled: true
        property bool compact: false
        readonly property double current: MicFilters.revision, MicFilters.value(moduleId, param.key)

        spacing: 0
        opacity: enabled ? 1 : 0.5

        RowLayout {
            visible: !row.compact
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
            enabled: row.enabled
            onMoved: MicFilters.setValue(row.moduleId, row.param.key, value)
            Accessible.name: row.param.label
            Accessible.description: MicFilters.format(row.moduleId, row.param.key, value)
        }
        QQC2.Label {
            visible: !row.compact && text !== ""
            text: row.param.description ?? ""
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            opacity: 0.7
        }
    }
}
