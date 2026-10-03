import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// Full-page block when PipeWire is unreachable or too old. The sidebar is disabled meanwhile.
Item {
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 30)
        spacing: Kirigami.Units.largeSpacing

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            icon.name: "dialog-error"
            text: i18nc("@title", "Rostrum needs PipeWire")
            explanation: App.pipewireDetail
        }

        QQC2.TextArea {
            Layout.fillWidth: true
            text: App.installHint
            readOnly: true
            wrapMode: Text.Wrap
            textFormat: TextEdit.PlainText
            font: Kirigami.Theme.fixedWidthFont
            selectByMouse: true
            Accessible.name: i18n("Packages to install")
        }

        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            text: i18nc("@action:button", "Retry")
            icon.name: "view-refresh"
            onClicked: App.retry()
        }
    }
}
