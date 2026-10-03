import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Radio list of outputs or inputs, shared by the Devices page and the wizard. The first row
// follows the system default. Outputs get a test tone button, inputs a live meter.
ColumnLayout {
    id: list

    property var devices: []
    property bool isInput: false
    property string selected   // saved choice; empty = system default
    property string inUse      // node actually used after fallback
    property var levels: ({})
    property string groupName

    signal picked(string name)

    readonly property var defaultRow: devices.find(d => d.isDefault)

    spacing: 0
    Accessible.role: Accessible.List
    Accessible.name: groupName

    QQC2.ButtonGroup {
        id: group
    }

    Repeater {
        model: [{ name: "", description: list.defaultRow
                      ? i18nc("@option:radio %1 is a device", "System default (%1)", list.defaultRow.description)
                      : i18nc("@option:radio", "System default"),
                  subtitle: i18nc("@info", "Follows the default device in System Settings"),
                  isDefault: false, headsetProfile: false, id: 0 }].concat(list.devices)

        delegate: QQC2.ItemDelegate {
            id: row
            required property var modelData
            required property int index
            readonly property bool chosen: list.selected === modelData.name
            readonly property bool active: modelData.name !== "" && modelData.name === list.inUse
            readonly property string toneTarget: modelData.name !== "" ? modelData.name
                                                  : (list.defaultRow ? list.defaultRow.name : "")

            Layout.fillWidth: true
            focusPolicy: Qt.StrongFocus
            onClicked: list.picked(modelData.name)
            Keys.onSpacePressed: list.picked(modelData.name)

            Accessible.role: Accessible.RadioButton
            Accessible.checkable: true
            Accessible.checked: chosen
            Accessible.name: modelData.description
            Accessible.description: modelData.subtitle

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
                        Kirigami.Icon {
                            visible: row.active
                            source: "rating"
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: implicitWidth
                            Accessible.name: list.isInput ? i18nc("@info", "Rostrum mic") : i18nc("@info", "Rostrum headphones")
                            QQC2.ToolTip.visible: starHover.hovered
                            QQC2.ToolTip.text: list.isInput ? i18nc("@info:tooltip", "Rostrum uses this mic")
                                                            : i18nc("@info:tooltip", "Rostrum plays your headphone mix here")
                            HoverHandler {
                                id: starHover
                            }
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
                        visible: list.isInput && row.modelData.name !== ""
                        orientation: Qt.Horizontal
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                        value: list.levels[row.modelData.name] ?? 0
                        accessibleName: i18nc("@label accessible", "%1 level", row.modelData.description)
                    }
                }

                QQC2.Button {
                    visible: !list.isInput && row.toneTarget !== ""
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
