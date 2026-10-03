import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// The preview of what Set Up OBS changes, shared by the OBS page and the setup wizard.
Kirigami.Dialog {
    id: previewDialog
    title: Obs.onlyDoubling ? i18nc("@title:dialog", "Fix OBS") : i18nc("@title:dialog", "Set Up OBS")
    padding: Kirigami.Units.largeSpacing
    preferredWidth: Kirigami.Units.gridUnit * 30
    standardButtons: Kirigami.Dialog.NoButton
    customFooterActions: [
        Kirigami.Action {
            text: i18nc("@action:button", "Apply")
            icon.name: "dialog-ok"
            enabled: Obs.canApply
            onTriggered: {
                Obs.apply()
                previewDialog.close()
            }
        },
        Kirigami.Action {
            text: i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            onTriggered: previewDialog.close()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: Obs.state === "closed"
                  ? i18n("OBS is closed. Rostrum will back up its scene collection, then change:")
                  : i18n("Rostrum will change these in OBS now:")
        }

        Repeater {
            model: Obs.planItems
            RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                QQC2.CheckBox {
                    id: box
                    Layout.alignment: Qt.AlignTop
                    checked: modelData.enabled
                    enabled: modelData.optional
                    onToggled: Obs.setPlanItemEnabled(modelData.index, checked)
                    Accessible.name: modelData.title
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: modelData.title
                        font.weight: Font.DemiBold
                        opacity: box.checked ? 1 : 0.6
                        TapHandler {
                            enabled: modelData.optional
                            onTapped: Obs.setPlanItemEnabled(modelData.index, !box.checked)
                        }
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: modelData.detail
                        opacity: 0.7
                    }
                }
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: i18n("Nothing is deleted. Muted sources stay in OBS, and Undo OBS Changes puts everything back.")
        }
    }
}
