import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Headphones and Stream faders for one playback bus, plus the link between them.
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
    property color accentColor: Kirigami.Theme.highlightColor
    property string busName: ""

    readonly property bool adjusting: phonesFader.hovered || phonesFader.activeFocus
                                      || streamFader.hovered || streamFader.activeFocus
                                      || linkButton.hovered || linkButton.activeFocus

    signal phonesEdited(real v)
    signal streamEdited(real v)
    signal linkToggled()
    signal muteRequested()
    signal soloRequested()
    signal destinationRequested(int index)
    signal menuRequested()
    signal renameRequested()

    implicitWidth: Kirigami.Units.gridUnit * 8
    implicitHeight: Kirigami.Units.gridUnit * 8

    function wire(fader) {
        fader.muteRequested.connect(root.muteRequested)
        fader.soloRequested.connect(root.soloRequested)
        fader.destinationRequested.connect(root.destinationRequested)
    }

    ColumnLayout {
        anchors.fill: parent
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
                Layout.preferredWidth: Kirigami.Units.gridUnit * 3.2
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
                Layout.preferredWidth: Kirigami.Units.gridUnit * 3.2
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
            icon.name: root.linked ? "link" : "edit-link"
            display: QQC2.AbstractButton.IconOnly
            onClicked: root.linkToggled()
            Accessible.name: root.linked ? i18nc("@action:button", "Unlink")
                                          : i18nc("@action:button", "Link Headphones and Stream")
            QQC2.ToolTip.text: Accessible.name
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
}
