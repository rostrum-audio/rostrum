import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

PlaceholderPage {
    title: App.mixReady ? i18nc("@title", "Mixer") : i18nc("@title", "No mix yet")
    iconName: "view-media-equalizer"
    explanation: App.mixReady ? i18n("The mix is running. Bus strips arrive in the next build.")
                              : i18n("Create the mix to add Rostrum's buses to PipeWire.")

    QQC2.Button {
        Layout.alignment: Qt.AlignHCenter
        visible: !App.mixReady
        enabled: !App.mixBusy
        text: App.mixBusy ? i18nc("@action:button in progress", "Creating Mix…") : i18nc("@action:button", "Create Mix")
        icon.name: "list-add"
        onClicked: App.createMix()
        QQC2.BusyIndicator {
            anchors.left: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height
            running: App.mixBusy
            visible: running
        }
    }
}
