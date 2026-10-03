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
                         : i18n("Mic disconnected. Plug it back in or choose another one.")
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
}
