import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Always visible: headphones, mic and PipeWire health. Device names open the Devices page.
QQC2.ToolBar {
    id: bar

    position: QQC2.ToolBar.Footer
    leftPadding: Kirigami.Units.smallSpacing
    rightPadding: Kirigami.Units.largeSpacing
    topPadding: 0
    bottomPadding: 0

    RowLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.smallSpacing

        QQC2.ToolButton {
            icon.name: App.headphonesMissing ? "dialog-warning" : "audio-headphones"
            text: i18nc("@info:status", "Headphones: %1", App.headphonesText)
            enabled: App.connected
            Layout.maximumWidth: bar.width / 3
            onClicked: applicationWindow().showPage("devices")
            QQC2.ToolTip.text: App.headphonesMissing ? i18n("Saved headphones are disconnected. Click to choose.")
                                                     : i18n("Choose headphones")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }

        QQC2.ToolButton {
            icon.name: App.micMissing ? "dialog-warning" : "audio-input-microphone"
            text: i18nc("@info:status", "Mic: %1", App.micText)
            enabled: App.connected
            Layout.maximumWidth: bar.width / 3
            onClicked: applicationWindow().showPage("devices")
            QQC2.ToolTip.text: App.micMissing ? i18n("Saved mic is disconnected. Click to choose.") : i18n("Choose mic")
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }

        Item {
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Accessible.role: Accessible.StaticText
            Accessible.name: App.pipewireStatusText
            Accessible.description: App.pipewireDetail

            Kirigami.Icon {
                source: App.pipewireState === "ok" ? "emblem-ok-symbolic"
                      : App.pipewireState === "degraded" ? "dialog-warning"
                      : App.pipewireState === "connecting" ? "view-refresh"
                      : "dialog-error"
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: implicitWidth
            }
            QQC2.Label {
                text: App.pipewireStatusText
            }

            HoverHandler {
                id: healthHover
            }
            QQC2.ToolTip.text: App.pipewireDetail
            QQC2.ToolTip.visible: healthHover.hovered && App.pipewireDetail.length > 0
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
}
