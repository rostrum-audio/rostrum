import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// "How do I sound?": a recorded check of the stream mic played back in the headphones, with a
// verdict on the level, and sidetone for hearing yourself live.
ColumnLayout {
    id: card

    readonly property bool busy: MicCheck.phase !== "idle"
    readonly property string peakText: MicCheck.peakDb > -100
                                       ? i18nc("@info level in decibels full scale", "%1 dBFS", MicCheck.peakDb.toFixed(1))
                                       : i18nc("@info no level at all", "silence")

    Layout.fillWidth: true
    spacing: 0

    FormCard.FormHeader {
        title: i18nc("@title:group", "Check Your Mic")
    }
    FormCard.FormCard {
        FormCard.AbstractFormDelegate {
            id: checkRow
            Layout.fillWidth: true
            background: null
            focusPolicy: Qt.NoFocus
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    QQC2.Label {
                        text: i18nc("@label", "Record and play back")
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    QQC2.Label {
                        text: {
                            switch (MicCheck.phase) {
                            case "recording":
                                return i18np("Recording. Talk the way you do on stream… %1 second left.",
                                             "Recording. Talk the way you do on stream… %1 seconds left.",
                                             Math.max(1, Math.ceil(MicCheck.seconds * (1 - MicCheck.progress))))
                            case "playing":
                                return i18n("Playing it back in your headphones.")
                            }
                            return i18np("Records %1 second of your stream mic, with its gain and filters, and plays it back in your headphones only. Viewers do not hear the playback.",
                                         "Records %1 seconds of your stream mic, with its gain and filters, and plays it back in your headphones only. Viewers do not hear the playback.",
                                         MicCheck.seconds)
                        }
                        font: Kirigami.Theme.smallFont
                        opacity: 0.7
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    QQC2.ProgressBar {
                        visible: card.busy
                        from: 0
                        to: 1
                        value: MicCheck.progress
                        Layout.fillWidth: true
                        Accessible.name: MicCheck.phase === "recording" ? i18nc("@label accessible", "Recording progress")
                                                                        : i18nc("@label accessible", "Playback progress")
                    }
                }
                QQC2.Button {
                    text: card.busy ? i18nc("@action:button", "Stop") : i18nc("@action:button", "Check Mic")
                    icon.name: card.busy ? "media-playback-stop" : "media-record"
                    enabled: card.busy || (App.connected && App.hasMic)
                    onClicked: card.busy ? MicCheck.stop() : MicCheck.start()
                }
            }
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.topMargin: 0
            visible: MicCheck.result !== "" && MicCheck.phase !== "recording"
            type: MicCheck.result === "good" ? Kirigami.MessageType.Positive
                : MicCheck.result === "silent" ? Kirigami.MessageType.Error
                                               : Kirigami.MessageType.Warning
            text: {
                switch (MicCheck.result) {
                case "silent":
                    return i18n("Rostrum heard almost nothing (loudest: %1). Check that the right mic is chosen on the Devices page and that it is not muted on the device itself.", card.peakText)
                case "quiet":
                    return i18n("Quiet: your loudest moment was %1. Raise the mic gain on the mixer, or move closer to the mic.", card.peakText)
                case "loud":
                    return i18n("Too loud: it reached %1 and may distort. Lower the mic gain on the mixer, or turn on the limiter.", card.peakText)
                }
                return i18n("Good level: your loudest moment was %1.", card.peakText)
            }
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormSwitchDelegate {
            text: i18nc("@option:check", "Hear yourself live")
            description: i18n("Your stream mic in your headphones as you talk (sidetone). It does not change what viewers hear.")
            checked: App.sidetoneEnabled
            onToggled: App.sidetoneEnabled = checked
        }
        FormCard.AbstractFormDelegate {
            visible: App.sidetoneEnabled
            Layout.fillWidth: true
            background: null
            focusPolicy: Qt.NoFocus
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                QQC2.Label {
                    text: i18nc("@label sidetone volume", "Volume")
                }
                QQC2.Slider {
                    id: sidetoneVolume
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: App.sidetoneVolume
                    wheelEnabled: Preferences.scrollToAdjust
                    onMoved: App.sidetoneVolume = value
                    Accessible.name: i18nc("@label accessible", "Sidetone volume")
                }
                QQC2.Label {
                    text: i18nc("@info percent", "%1%", Math.round(sidetoneVolume.value * 100))
                    font.features: { "tnum": 1 }
                    opacity: 0.8
                }
            }
        }
    }
}
