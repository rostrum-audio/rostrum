import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Radio list of outputs or inputs, shared by the Devices page and the wizard. The first row
// follows the system default. Outputs get a test tone button, inputs a live meter.
// Rows are selectable cards (Penpot). A chosen device that is unplugged stays selected and is
// marked Disconnected; a temporary fallback is marked on the stand-in row.
ColumnLayout {
    id: list

    property var devices: []
    property bool isInput: false
    property string selected   // saved choice; empty = system default
    property string inUse      // node actually used after fallback
    property var levels: ({})
    property string groupName
    // True when the saved device is unplugged (App.headphonesMissing / App.micMissing).
    property bool savedMissing: false
    // Mic list only: Devices.micFallback. When false and saved is missing, other mics are
    // available but not standing in.
    property bool allowFallback: true

    signal picked(string name)

    readonly property var defaultRow: devices.find(d => d.isDefault)
    readonly property string missingLabel: list.isInput ? App.missingMicLabel
                                                        : App.missingHeadphonesLabel
    readonly property bool usingTemporary: {
        if (!list.savedMissing || list.inUse === "") {
            return false
        }
        // Saved explicit device: temporary when a different node is actually routed.
        if (list.selected !== "") {
            return list.inUse !== list.selected
        }
        // System default selected: temporary when in-use is not the current default node.
        return !list.defaultRow || list.inUse !== list.defaultRow.name
    }
    // System default, then a Disconnected card for the saved choice when unplugged, then live devices.
    readonly property var rows: {
        const head = [{
            name: "",
            description: list.defaultRow
                         ? i18nc("@option:radio %1 is a device", "System default (%1)", list.defaultRow.description)
                         : i18nc("@option:radio", "System default"),
            subtitle: i18nc("@info", "Follows the default device in System Settings"),
            isDefault: false,
            headsetProfile: false,
            id: 0,
            disconnected: false
        }]
        const missing = []
        if (list.savedMissing && list.selected !== "") {
            const label = list.missingLabel.length > 0 ? list.missingLabel : list.selected
            missing.push({
                name: list.selected,
                description: label,
                subtitle: "",
                isDefault: false,
                headsetProfile: false,
                id: 0,
                disconnected: true
            })
        }
        return head.concat(missing).concat(list.devices)
    }

    spacing: Kirigami.Units.smallSpacing
    Accessible.role: Accessible.List
    Accessible.name: groupName

    QQC2.ButtonGroup {
        id: group
    }

    Repeater {
        model: list.rows

        delegate: QQC2.ItemDelegate {
            id: row
            required property var modelData
            required property int index
            readonly property bool chosen: list.selected === modelData.name
            readonly property bool isDisconnected: modelData.disconnected === true
            // True when this row is what Rostrum is actually routed to. System default owns
            // the pill when that radio is selected; otherwise the named node does.
            readonly property bool isInUse: {
                if (row.isDisconnected || list.inUse === "") {
                    return false
                }
                if (modelData.name === "") {
                    return list.selected === ""
                           && list.defaultRow
                           && list.defaultRow.name === list.inUse
                }
                if (modelData.name !== list.inUse) {
                    return false
                }
                // Named default node: hide when System default radio owns the indicator.
                if (list.selected === "" && list.defaultRow
                        && list.defaultRow.name === list.inUse) {
                    return false
                }
                return true
            }
            readonly property bool isTemporaryStandIn: {
                if (row.isDisconnected || !list.usingTemporary || list.inUse === "") {
                    return false
                }
                if (modelData.name === "") {
                    // System default owns the pill only when that radio is selected.
                    return list.selected === ""
                           && list.defaultRow
                           && list.defaultRow.name === list.inUse
                }
                if (modelData.name !== list.inUse) {
                    return false
                }
                if (list.selected === "" && list.defaultRow
                        && list.defaultRow.name === list.inUse) {
                    return false
                }
                return true
            }
            readonly property bool availableFallbackOff: list.isInput && list.savedMissing
                                                         && !list.allowFallback
                                                         && modelData.name !== ""
                                                         && modelData.name !== list.selected
                                                         && !row.isDisconnected
            readonly property string toneTarget: modelData.name !== "" ? modelData.name
                                                  : (list.defaultRow ? list.defaultRow.name : "")
            // Disconnected / fallback pills first; plain "In use" when routed here.
            readonly property string statusText: {
                if (row.isDisconnected) {
                    return i18nc("@info chosen device unplugged", "Disconnected")
                }
                if (row.isTemporaryStandIn) {
                    return i18nc("@info device is temporary fallback", "In use until yours returns")
                }
                if (row.availableFallbackOff) {
                    return i18nc("@info other mic while fallback off", "Available (fallback off)")
                }
                if (row.isInUse) {
                    return i18nc("@info device Rostrum is using", "In use")
                }
                return ""
            }
            readonly property bool isPlainInUse: row.isInUse && !row.isTemporaryStandIn
                                                 && !row.availableFallbackOff
                                                 && !row.isDisconnected

            Layout.fillWidth: true
            focusPolicy: Qt.StrongFocus
            // Card chrome (Penpot device tiles).
            background: Rectangle {
                radius: Kirigami.Units.cornerRadius
                color: row.highlighted || row.chosen
                       ? Qt.alpha(Kirigami.Theme.highlightColor, row.highlighted ? 1 : 0.12)
                       : Kirigami.Theme.backgroundColor
                border.width: 1
                border.color: row.chosen || row.highlighted
                              ? Kirigami.Theme.highlightColor
                              : Qt.alpha(Kirigami.Theme.textColor, 0.15)
            }
            highlighted: false
            onClicked: list.picked(modelData.name)
            Keys.onSpacePressed: list.picked(modelData.name)

            Accessible.role: Accessible.RadioButton
            Accessible.checkable: true
            Accessible.checked: chosen
            Accessible.name: modelData.description
            Accessible.description: {
                const parts = [modelData.subtitle, row.statusText].filter(s => s)
                return parts.join(", ")
            }

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                QQC2.RadioButton {
                    checked: row.chosen
                    focusPolicy: Qt.NoFocus
                    QQC2.ButtonGroup.group: group
                    onClicked: list.picked(row.modelData.name)
                    Accessible.ignored: true
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: row.modelData.description
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            font.weight: row.chosen ? Font.DemiBold : Font.Normal
                        }
                        // In-use / disconnected / temporary / fallback-off pill.
                        Rectangle {
                            id: statusPill
                            visible: row.statusText.length > 0
                            radius: height / 2
                            color: row.isDisconnected
                                   ? Qt.alpha(Kirigami.Theme.negativeTextColor, 0.2)
                                   : row.isTemporaryStandIn
                                     ? Qt.alpha(Kirigami.Theme.neutralTextColor, 0.2)
                                     : Qt.alpha(Kirigami.Theme.textColor, 0.08)
                            implicitWidth: statusLabel.implicitWidth + Kirigami.Units.smallSpacing * 2
                            implicitHeight: statusLabel.implicitHeight + Kirigami.Units.smallSpacing
                            Accessible.name: row.isPlainInUse
                                             ? (list.isInput ? i18nc("@info", "Rostrum mic")
                                                             : i18nc("@info", "Rostrum headphones"))
                                             : ""
                            QQC2.Label {
                                id: statusLabel
                                anchors.centerIn: parent
                                text: row.statusText
                                font: Kirigami.Theme.smallFont
                                color: row.isDisconnected ? Kirigami.Theme.negativeTextColor
                                                          : row.isTemporaryStandIn
                                                            ? Kirigami.Theme.neutralTextColor
                                                            : Kirigami.Theme.textColor
                                opacity: 0.9
                            }
                            HoverHandler {
                                id: inUseHover
                                enabled: row.isPlainInUse
                            }
                            QQC2.ToolTip.visible: inUseHover.hovered
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            QQC2.ToolTip.text: list.isInput
                                               ? i18nc("@info:tooltip", "Rostrum uses this mic")
                                               : i18nc("@info:tooltip", "Rostrum plays your headphone mix here")
                        }
                    }
                    QQC2.Label {
                        visible: text.length > 0
                        text: {
                            const parts = []
                            if (row.modelData.subtitle) {
                                parts.push(row.modelData.subtitle)
                            }
                            if (Preferences.showNodeIds && row.modelData.name) {
                                parts.push(i18nc("@info node name and id", "%1 (#%2)", row.modelData.name, row.modelData.id))
                            }
                            return parts.join(" · ")
                        }
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        font: Kirigami.Theme.smallFont
                        opacity: 0.7
                    }
                    PeakMeter {
                        visible: list.isInput && row.modelData.name !== "" && !row.isDisconnected
                        orientation: Qt.Horizontal
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                        value: list.levels[row.modelData.name] ?? 0
                        accessibleName: i18nc("@label accessible", "%1 level", row.modelData.description)
                    }
                }

                QQC2.Button {
                    visible: !list.isInput && row.toneTarget !== "" && !row.isDisconnected
                    icon.name: Devices.toneTarget === row.toneTarget ? "media-playback-playing" : "media-playback-start"
                    text: i18nc("@action:button", "Test")
                    display: QQC2.AbstractButton.TextBesideIcon
                    onClicked: Devices.playTone(row.toneTarget)
                    Accessible.name: i18nc("@action:button accessible", "Play a test chime on %1", row.modelData.description)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: i18nc("@info:tooltip", "Play a short chime: left, right, then both")
                }
            }
        }
    }
}
