import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Headphones and Stream faders for one playback bus, plus the link between them. A third fader
// sets how loud the bus is in voice chat; it has its own mute button and is never linked.
Item {
    id: root

    property real phonesValue: 1.0
    property real streamValue: 1.0
    property bool linked: true
    property bool phonesEnabled: true
    property bool streamEnabled: true
    property real phonesPeak: 0
    property real streamPeak: 0
    property bool phonesClip: false
    property bool streamClip: false
    property bool chatShown: false
    property real chatValue: 0.7
    property bool chatEnabled: true
    property bool chatMuted: false // the mute button; chatEnabled is also false while the bus is muted
    property real chatPeak: 0 // no chat meter yet
    property color accentColor: Kirigami.Theme.highlightColor
    property string busName: ""

    readonly property bool adjusting: phonesFader.hovered || phonesFader.activeFocus
                                      || streamFader.hovered || streamFader.activeFocus
                                      || linkButton.hovered || linkButton.activeFocus
                                      || (chatShown && (chatFader.hovered || chatFader.activeFocus))

    signal phonesEdited(real v)
    signal streamEdited(real v)
    signal chatMoved(real position)
    signal chatMuteToggled()
    signal linkToggled()
    signal muteRequested()
    signal soloRequested()
    signal destinationRequested(int index)
    signal menuRequested()
    signal renameRequested()

    // The cap and its dB labels need 2.8 grid units; two faders have room to spare, three do not.
    readonly property real faderWidth: Kirigami.Units.gridUnit * (chatShown ? 2.8 : 3.2)

    implicitWidth: Kirigami.Units.gridUnit * 8
    implicitHeight: Kirigami.Units.gridUnit * 8

    function wire(fader) {
        fader.muteRequested.connect(root.muteRequested)
        fader.soloRequested.connect(root.soloRequested)
        fader.destinationRequested.connect(root.destinationRequested)
    }

    RowLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.smallSpacing

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.smallSpacing

                PeakMeter {
                    id: phonesMeter
                    Layout.fillHeight: true
                    Layout.topMargin: Math.max(0, phonesFader.topPadding + phonesFader.capHeight / 2 - phonesMeter.clipSize - 1)
                    Layout.bottomMargin: phonesFader.bottomPadding + phonesFader.capHeight / 2
                    value: root.phonesPeak
                    clip: root.phonesClip
                    opacity: root.phonesEnabled ? 1 : 0.45
                    accessibleName: i18nc("@label accessible", "%1 headphones level", root.busName)
                }
                Fader {
                    id: phonesFader
                    objectName: "phonesFader"
                    Layout.fillHeight: true
                    Layout.preferredWidth: root.faderWidth
                    orientation: Qt.Vertical
                    enabled: root.phonesEnabled
                    value: root.phonesValue
                    accentColor: root.accentColor
                    dimmed: !root.phonesEnabled
                    accessibleName: i18nc("@label accessible", "%1 headphones", root.busName)
                    onEdited: v => { if (enabled) root.phonesEdited(v) }
                    Keys.onMenuPressed: root.menuRequested()
                    Keys.onReleased: event => {
                        if (event.key === Qt.Key_F2) {
                            root.renameRequested()
                            event.accepted = true
                        }
                    }
                    Component.onCompleted: root.wire(phonesFader)
                }
                Fader {
                    id: streamFader
                    objectName: "streamFader"
                    Layout.fillHeight: true
                    Layout.preferredWidth: root.faderWidth
                    orientation: Qt.Vertical
                    enabled: root.streamEnabled
                    value: root.streamValue
                    accentColor: root.accentColor
                    dimmed: !root.streamEnabled
                    accessibleName: i18nc("@label accessible", "%1 stream", root.busName)
                    onEdited: v => { if (enabled) root.streamEdited(v) }
                    Keys.onMenuPressed: root.menuRequested()
                    Keys.onReleased: event => {
                        if (event.key === Qt.Key_F2) {
                            root.renameRequested()
                            event.accepted = true
                        }
                    }
                    Component.onCompleted: root.wire(streamFader)
                }
                PeakMeter {
                    id: streamMeter
                    Layout.fillHeight: true
                    Layout.topMargin: Math.max(0, streamFader.topPadding + streamFader.capHeight / 2 - streamMeter.clipSize - 1)
                    Layout.bottomMargin: streamFader.bottomPadding + streamFader.capHeight / 2
                    value: root.streamPeak
                    clip: root.streamClip
                    opacity: root.streamEnabled ? 1 : 0.45
                    accessibleName: i18nc("@label accessible", "%1 stream level", root.busName)
                }
            }

            QQC2.ToolButton {
                id: linkButton
                objectName: "linkButton"
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium + Kirigami.Units.smallSpacing * 2
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium + Kirigami.Units.smallSpacing * 2
                // Breeze's "link" / "edit-link" are identical; qrc SVGs need Kirigami.Icon
                // for theme tinting — ToolButton.icon.color does not recolor them reliably.
                display: QQC2.AbstractButton.IconOnly
                padding: Kirigami.Units.smallSpacing
                onClicked: root.linkToggled()
                Accessible.name: root.linked ? i18nc("@action:button", "Unlink")
                                              : i18nc("@action:button", "Link Headphones and Stream")
                QQC2.ToolTip.text: Accessible.name
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

                contentItem: Kirigami.Icon {
                    source: root.linked ? "qrc:/icons/mix-link.svg"
                                        : "qrc:/icons/mix-unlink.svg"
                    // Linked = full text color; unlinked = quieter + broken glyph.
                    color: root.linked ? Kirigami.Theme.textColor
                                       : Kirigami.Theme.disabledTextColor
                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                    implicitHeight: Kirigami.Units.iconSizes.smallMedium
                    isMask: true
                }
            }
        }

        // Voice chat level: same height as the other faders, the mute button sits where the link button does.
        ColumnLayout {
            visible: root.chatShown
            Layout.fillHeight: true
            spacing: Kirigami.Units.smallSpacing

            Fader {
                id: chatFader
                objectName: "chatFader"
                Layout.fillHeight: true
                Layout.preferredWidth: root.faderWidth
                orientation: Qt.Vertical
                enabled: root.chatEnabled
                value: root.chatValue
                resetValue: 0.7
                accentColor: root.accentColor
                dimmed: !root.chatEnabled
                accessibleName: i18nc("@label accessible", "%1 voice chat", root.busName)
                QQC2.ToolTip.text: i18nc("@label fader", "Voice chat")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                onEdited: v => { if (enabled) root.chatMoved(v) }
                Keys.onMenuPressed: root.menuRequested()
                Keys.onReleased: event => {
                    if (event.key === Qt.Key_F2) {
                        root.renameRequested()
                        event.accepted = true
                    }
                }
                Component.onCompleted: root.wire(chatFader)
            }
            QQC2.ToolButton {
                id: chatMuteButton
                objectName: "chatMuteButton"
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium + Kirigami.Units.smallSpacing * 2
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium + Kirigami.Units.smallSpacing * 2
                display: QQC2.AbstractButton.IconOnly
                icon.name: "audio-volume-muted"
                checkable: true
                checked: root.chatMuted
                onClicked: {
                    root.chatMuteToggled()
                    checked = Qt.binding(() => root.chatMuted) // the bus answers; a click must not keep the state
                }
                Accessible.name: i18nc("@action:button", "%1: Mute sounds in voice chat", root.busName)
                QQC2.ToolTip.text: Accessible.name
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }
}
