import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: section
    required property var setup
    spacing: Kirigami.Units.smallSpacing

    Kirigami.Heading {
        level: 3
        text: i18nc("@title", "Recording tracks")
        Accessible.name: text
    }
    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: i18nc("@info", "Keep the stream and Twitch VOD mixes on tracks 1 and 2. Choose separate buses for recording on tracks 3–6. Headphones-only buses can be recorded without changing their stream destination.")
        Accessible.name: text
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: section.setup.collection.length > 0
        text: i18nc("@info", "Scene collection: %1", section.setup.collection)
        Accessible.name: text
        wrapMode: Text.WordWrap
        opacity: 0.7
    }
    Repeater {
        model: section.setup.rows
        delegate: ColumnLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            RowLayout {
                Layout.fillWidth: true
                QQC2.Label {
                    text: i18nc("@label", "Track %1", modelData.track)
                    Accessible.name: text
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                }
                QQC2.ComboBox {
                    objectName: "recordingBus" + modelData.track
                    Layout.fillWidth: true
                    model: section.setup.choices
                    textRole: "label"
                    valueRole: "id"
                    currentIndex: {
                        for (let n = 0; n < model.length; ++n)
                            if (model[n].id === modelData.busId)
                                return n
                        return 0
                    }
                    enabled: section.setup.canPreview
                    onActivated: section.setup.choose(modelData.track, currentValue)
                    Accessible.name: i18nc("@label accessible", "Bus for recording track %1", modelData.track)
                }
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: modelData.current
                Accessible.name: text
                opacity: 0.7
            }
        }
    }
    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: section.setup.status
        Accessible.name: text
    }
    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.7
        text: i18nc("@info", "Bus faders and mutes affect their recordings. Mic mute silences the mic track. Panic mute and Stream master mute affect the combined mixes; isolated playback tracks keep recording.")
        Accessible.name: text
    }
    Flow {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        QQC2.Button {
            objectName: "reviewRecording"
            text: i18nc("@action:button", "Review Recording Changes…")
            Accessible.name: text
            enabled: section.setup.canPreview
            onClicked: section.setup.preview()
        }
        QQC2.Button {
            objectName: "undoRecording"
            text: i18nc("@action:button", "Undo Recording Changes")
            Accessible.name: text
            enabled: section.setup.canUndo
            onClicked: section.setup.undo()
        }
    }
    Connections {
        target: section.setup
        function onPreviewReady() { preview.open() }
    }
    Kirigami.Dialog {
        id: preview
        title: i18nc("@title:dialog", "Recording tracks")
        preferredWidth: Kirigami.Units.gridUnit * 32
        padding: Kirigami.Units.largeSpacing
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Apply Recording Changes")
                enabled: section.setup.canApply
                onTriggered: {
                    section.setup.apply()
                    preview.close()
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Cancel")
                onTriggered: preview.close()
            }
        ]
        ColumnLayout {
            Accessible.name: preview.title
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18nc("@info", "Rostrum will back up these assignments before changing OBS. Encoder settings and stream destinations stay as they are.")
                Accessible.name: text
            }
            Repeater {
                model: section.setup.previewItems
                QQC2.Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: modelData
                    Accessible.name: text
                }
            }
        }
    }
}
