import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Page banners for trouble that does not block the app. No modals for these.
ColumnLayout {
    spacing: 0

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
        type: Kirigami.MessageType.Warning
        visible: App.headphonesMissing
        text: i18n("Headphones disconnected, scene held. Playing through %1 until they come back.", App.headphonesText)
        actions: Kirigami.Action {
            text: i18nc("@action:button", "Choose Headphones")
            icon.name: "audio-headphones"
            onTriggered: applicationWindow().showPage("devices")
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
        type: Kirigami.MessageType.Warning
        visible: App.micMissing
        text: App.hasMic ? i18n("Mic disconnected. Using %1 until it comes back.", App.micText)
                         : i18n("Mic disconnected. Your stream mic is silent until it comes back.")
        actions: Kirigami.Action {
            text: i18nc("@action:button", "Choose Mic")
            icon.name: "audio-input-microphone"
            onTriggered: applicationWindow().showPage("devices")
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
        type: Kirigami.MessageType.Warning
        visible: App.connected && !App.hasWirePlumber
        text: i18n("The session manager is not WirePlumber. Rostrum still works while it is open, but app routes may not survive a reboot, so rule export is off.")
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
        type: Kirigami.MessageType.Error
        visible: App.connected && App.mixError.length > 0
        text: App.mixError
        actions: Kirigami.Action {
            text: i18nc("@action:button", "Try Again")
            icon.name: "view-refresh"
            onTriggered: App.createMix()
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
        type: Updates.state === "ready" ? Kirigami.MessageType.Positive : Kirigami.MessageType.Information
        visible: Updates.showBanner || Updates.state === "downloading"
        text: Updates.state === "ready"
              ? i18n("Rostrum %1 is installed. Restart Rostrum to start using it; your mix keeps playing meanwhile.", Updates.latestVersion)
              : Updates.state === "downloading"
                ? i18n("Downloading Rostrum %1… %2%", Updates.latestVersion, Math.round(Updates.progress * 100))
                : Updates.installKind === "appimage" && Updates.canInstall
                  ? i18n("Rostrum %1 is available.", Updates.latestVersion)
                  : Updates.installKind === "package"
                    ? i18n("Rostrum %1 is available. Update it from your software center or package manager.", Updates.latestVersion)
                    : i18n("Rostrum %1 is available. Pull and rebuild the source to update.", Updates.latestVersion)
        actions: [
            Kirigami.Action {
                visible: Updates.state === "ready"
                text: i18nc("@action:button", "Restart Now")
                icon.name: "view-refresh"
                onTriggered: Updates.restart()
            },
            Kirigami.Action {
                visible: Updates.state === "available" && Updates.canInstall
                text: i18nc("@action:button", "Install")
                icon.name: "download"
                onTriggered: Updates.install()
            },
            Kirigami.Action {
                visible: Updates.state === "available" && Updates.releaseUrl.toString() !== ""
                text: i18nc("@action:button", "What's New")
                icon.name: "documentinfo"
                onTriggered: Qt.openUrlExternally(Updates.releaseUrl)
            },
            Kirigami.Action {
                visible: Updates.state === "available"
                text: i18nc("@action:button", "Skip This Version")
                icon.name: "dialog-cancel"
                onTriggered: Updates.skipThisVersion()
            }
        ]
    }
}
