import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Stand-in for pages that later slices fill in.
QQC2.Pane {
    id: page

    property string title
    property string iconName
    property string explanation
    default property alias extra: column.data

    padding: Kirigami.Units.gridUnit
    focusPolicy: Qt.StrongFocus
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
    }

    ColumnLayout {
        id: column
        anchors.centerIn: parent
        width: Math.min(parent.width, Kirigami.Units.gridUnit * 28)

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            icon.name: page.iconName
            text: page.title
            explanation: page.explanation
        }
    }
}
