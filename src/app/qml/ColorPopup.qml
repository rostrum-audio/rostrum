import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

// The 12 bus swatches. Bus colors are user data, not the Plasma accent.
QQC2.Popup {
    id: popup

    property string current
    signal picked(string color)

    padding: Kirigami.Units.largeSpacing
    focus: true

    contentItem: GridLayout {
        columns: 6
        rowSpacing: Kirigami.Units.smallSpacing
        columnSpacing: Kirigami.Units.smallSpacing

        Repeater {
            model: Mixer.palette
            QQC2.AbstractButton {
                required property string modelData
                required property int index
                implicitWidth: Kirigami.Units.gridUnit * 1.6
                implicitHeight: implicitWidth
                focusPolicy: Qt.StrongFocus
                Accessible.role: Accessible.Button
                Accessible.name: Mixer.paletteNames[index]
                QQC2.ToolTip.text: Mixer.paletteNames[index]
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                hoverEnabled: true
                background: Rectangle {
                    radius: width / 2
                    color: modelData
                    border.width: parent.visualFocus || modelData.toLowerCase() === popup.current.toLowerCase() ? 3 : 0
                    border.color: parent.visualFocus ? Kirigami.Theme.focusColor : Kirigami.Theme.textColor
                }
                onClicked: {
                    popup.picked(modelData)
                    popup.close()
                }
            }
        }
    }
}
