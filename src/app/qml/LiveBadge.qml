import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// LIVE and REC with the time since OBS started them. Only shown while OBS streams or records.
RowLayout {
    id: badges

    signal clicked()

    property double now: Date.now()

    function elapsed(ms) {
        const total = Math.max(0, Math.floor(ms / 1000))
        const h = Math.floor(total / 3600)
        const m = Math.floor(total / 60) % 60
        const s = total % 60
        const pad = n => n < 10 ? "0" + n : "" + n
        return h > 0 ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s)
    }

    readonly property string streamTime: Obs.streaming ? elapsed(now - Obs.streamStartMs) : ""
    readonly property string recordTime: !Obs.recording ? ""
                                         : Obs.recordPaused ? elapsed(Obs.recordPausedElapsedMs)
                                         : elapsed(now - Obs.recordStartMs)

    visible: Obs.streaming || Obs.recording
    spacing: Kirigami.Units.smallSpacing

    Timer {
        interval: 1000
        repeat: true
        running: badges.visible && (Obs.streaming || (Obs.recording && !Obs.recordPaused))
        triggeredOnStart: true
        onTriggered: badges.now = Date.now()
    }

    QQC2.AbstractButton {
        id: liveBadge
        visible: Obs.streaming
        focusPolicy: Qt.TabFocus
        padding: Kirigami.Units.smallSpacing
        leftPadding: Kirigami.Units.smallSpacing * 2
        rightPadding: Kirigami.Units.smallSpacing * 2
        onClicked: badges.clicked()
        Accessible.role: Accessible.Button
        Accessible.name: i18nc("@info accessible", "Live on stream for %1", badges.streamTime)
        Accessible.description: i18nc("@info accessible", "Opens the OBS page")
        QQC2.ToolTip.text: i18nc("@info:tooltip", "OBS is streaming")
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        hoverEnabled: true

        // Darker than the logo's coral so white text stays readable.
        background: Rectangle {
            radius: height / 2
            color: "#C8102E"
            border.width: liveBadge.visualFocus ? 2 : 0
            border.color: Kirigami.Theme.focusColor
        }
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Rectangle {
                implicitWidth: 8
                implicitHeight: 8
                radius: 4
                color: "white"
            }
            QQC2.Label {
                text: i18nc("@info badge, keep short", "LIVE")
                color: "white"
                font.weight: Font.Bold
                font.letterSpacing: 0.5
            }
            QQC2.Label {
                text: badges.streamTime
                color: "white"
            }
        }
    }

    QQC2.AbstractButton {
        id: recBadge
        visible: Obs.recording
        focusPolicy: Qt.TabFocus
        padding: Kirigami.Units.smallSpacing
        leftPadding: Kirigami.Units.smallSpacing * 2
        rightPadding: Kirigami.Units.smallSpacing * 2
        onClicked: badges.clicked()
        Accessible.role: Accessible.Button
        Accessible.name: Obs.recordPaused ? i18nc("@info accessible", "Recording paused at %1", badges.recordTime)
                                          : i18nc("@info accessible", "Recording for %1", badges.recordTime)
        Accessible.description: i18nc("@info accessible", "Opens the OBS page")
        QQC2.ToolTip.text: Obs.recordPaused ? i18nc("@info:tooltip", "OBS recording is paused") : i18nc("@info:tooltip", "OBS is recording")
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        hoverEnabled: true

        background: Rectangle {
            radius: height / 2
            color: "transparent"
            border.width: recBadge.visualFocus ? 2 : 1
            border.color: recBadge.visualFocus ? Kirigami.Theme.focusColor : Qt.alpha(Kirigami.Theme.textColor, 0.35)
        }
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            StreamDot {
                size: 8
                opacity: Obs.recordPaused ? 0.4 : 1
            }
            QQC2.Label {
                text: Obs.recordPaused ? i18nc("@info badge, keep short", "REC paused") : i18nc("@info badge, keep short", "REC")
                font.weight: Font.Bold
                font.letterSpacing: 0.5
            }
            QQC2.Label {
                text: badges.recordTime
            }
        }
    }
}
