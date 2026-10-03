import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// The biggest control in the chrome. Live is neutral, Muted is filled red, No mic is dim with a
// warning icon. Click toggles mute; right-click, long-press or the Menu key opens gain and sidetone.
QQC2.AbstractButton {
    id: mic

    signal devicesRequested()

    readonly property bool noMic: !App.hasMic
    readonly property bool muted: App.micMuted

    implicitHeight: Kirigami.Units.gridUnit * 2.25
    implicitWidth: Math.max(Kirigami.Units.gridUnit * 8, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: Kirigami.Units.largeSpacing * 1.5
    rightPadding: Kirigami.Units.largeSpacing * 1.5
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    text: noMic ? i18nc("@action:button mic state", "No mic")
                : muted ? i18nc("@action:button mic state", "Mic muted")
                        : i18nc("@action:button mic state", "Mic live")

    Accessible.role: Accessible.Button
    Accessible.name: i18nc("@action:button", "Mic mute")
    Accessible.checkable: true
    Accessible.checked: muted
    Accessible.description: text

    onClicked: App.toggleMicMute()
    onPressAndHold: popup.open()
    Keys.onMenuPressed: popup.open()

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: popup.open()
    }

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius
        color: mic.muted && !mic.noMic ? Kirigami.Theme.negativeTextColor
             : mic.down ? Qt.alpha(Kirigami.Theme.textColor, 0.15)
             : mic.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.08)
             : "transparent"
        border.width: mic.visualFocus ? 2 : 1
        border.color: mic.visualFocus ? Kirigami.Theme.focusColor
                    : mic.muted && !mic.noMic ? Kirigami.Theme.negativeTextColor
                    : Qt.alpha(Kirigami.Theme.textColor, 0.3)
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        opacity: mic.noMic ? 0.6 : 1
        Kirigami.Icon {
            source: mic.noMic ? "dialog-warning" : mic.muted ? "microphone-sensitivity-muted" : "microphone-sensitivity-high"
            color: mic.muted && !mic.noMic ? "white" : Kirigami.Theme.textColor
            isMask: !mic.noMic
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
        }
        QQC2.Label {
            text: mic.text
            font.weight: Font.Bold
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.1
            color: mic.muted && !mic.noMic ? "white" : Kirigami.Theme.textColor
        }
    }

    QQC2.ToolTip.text: noMic ? i18n("No mic found. Right-click for options.")
                             : i18n("Click to mute or unmute. Right-click for gain and sidetone.")
    QQC2.ToolTip.visible: hovered && !popup.visible
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

    QQC2.Popup {
        id: popup
        y: mic.height + Kirigami.Units.smallSpacing
        x: mic.width - width
        padding: Kirigami.Units.largeSpacing
        focus: true

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing
            width: Kirigami.Units.gridUnit * 16

            Kirigami.Heading {
                level: 4
                text: i18nc("@title", "Mic")
            }
            QQC2.Label {
                text: App.micText
                opacity: 0.7
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            QQC2.Label {
                text: i18nc("@label", "Gain")
                Layout.topMargin: Kirigami.Units.smallSpacing
            }
            QQC2.Slider {
                id: gain
                from: 0
                to: 1.5
                value: App.micGain
                stepSize: 0.01
                Layout.fillWidth: true
                focus: true
                Accessible.name: i18nc("@label", "Mic gain")
                onMoved: App.micGain = value
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: App.micGain = 1.0
                }
            }

            QQC2.Switch {
                id: sidetone
                text: i18nc("@option:check", "Sidetone (hear yourself)")
                checked: App.sidetoneEnabled
                onToggled: App.sidetoneEnabled = checked
                Layout.topMargin: Kirigami.Units.smallSpacing
            }
            QQC2.Slider {
                from: 0
                to: 1
                stepSize: 0.01
                value: App.sidetoneVolume
                enabled: App.sidetoneEnabled
                Layout.fillWidth: true
                Accessible.name: i18nc("@label", "Sidetone volume")
                onMoved: App.sidetoneVolume = value
            }

            QQC2.Button {
                text: i18nc("@action:button", "Choose Mic…")
                icon.name: "audio-input-microphone"
                Layout.topMargin: Kirigami.Units.smallSpacing
                onClicked: {
                    popup.close()
                    mic.devicesRequested()
                }
            }
        }
    }
}
