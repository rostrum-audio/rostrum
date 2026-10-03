import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// The biggest control in the chrome. Live is neutral, Muted is filled red, No mic is dim with a
// warning icon, and Offline (no PipeWire) is dim and disabled. Click toggles mute; right-click, long-press or the Menu key opens gain and sidetone.
QQC2.AbstractButton {
    id: mic

    signal devicesRequested()

    readonly property bool offline: !App.connected
    readonly property bool noMic: !App.hasMic
    readonly property bool muted: App.micMuted

    implicitHeight: Kirigami.Units.gridUnit * 2.25
    implicitWidth: Math.max(Kirigami.Units.gridUnit * 8, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: Kirigami.Units.largeSpacing * 1.5
    rightPadding: Kirigami.Units.largeSpacing * 1.5
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    enabled: !offline

    text: offline ? i18nc("@action:button mic state", "Mic offline")
        : noMic ? i18nc("@action:button mic state", "No mic")
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

    readonly property bool live: !noMic && !muted

    background: Rectangle {
        radius: height / 2
        color: mic.muted && !mic.noMic ? (mic.down ? Qt.darker(Kirigami.Theme.negativeTextColor, 1.15)
                                                   : Kirigami.Theme.negativeTextColor)
             : mic.down ? Qt.alpha(Kirigami.Theme.textColor, 0.16)
             : mic.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.1)
             : mic.live ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.08)
             : Qt.alpha(Kirigami.Theme.textColor, 0.05)
        border.width: mic.visualFocus ? 2 : 1
        border.color: mic.visualFocus ? Kirigami.Theme.focusColor
                    : mic.muted && !mic.noMic ? Kirigami.Theme.negativeTextColor
                    : mic.live ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.45)
                    : Qt.alpha(Kirigami.Theme.textColor, 0.25)
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing * 1.5
        opacity: mic.noMic ? 0.6 : 1
        Rectangle {
            visible: mic.live
            implicitWidth: Kirigami.Units.smallSpacing * 2
            implicitHeight: implicitWidth
            radius: width / 2
            color: Kirigami.Theme.positiveTextColor
        }
        Kirigami.Icon {
            source: mic.offline ? "microphone-sensitivity-muted" : mic.noMic ? "dialog-warning" : mic.muted ? "microphone-sensitivity-muted" : "microphone-sensitivity-high"
            color: mic.muted && !mic.noMic ? "white" : Kirigami.Theme.textColor
            isMask: mic.offline || !mic.noMic
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
            PlainSlider {
                id: gain
                from: 0
                to: 1.5
                value: App.micGain
                Layout.fillWidth: true
                focus: true
                Accessible.name: i18nc("@label", "Mic gain")
                onMoved: App.micGain = value
                SliderReset {
                    target: gain
                    onTriggered: App.micGain = 1.0
                }
            }

            QQC2.Switch {
                id: sidetone
                text: i18nc("@option:check", "Sidetone (hear yourself)")
                checked: App.sidetoneEnabled
                onToggled: App.sidetoneEnabled = checked
                Layout.topMargin: Kirigami.Units.smallSpacing
            }
            PlainSlider {
                from: 0
                to: 1
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
