import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Phones / Stream / Both as one segmented control, not a buried menu. The chosen segment is
// filled and bold, so color is never the only signal.
RowLayout {
    id: control

    property int current: 2
    property string busName

    signal picked(int index)

    spacing: 0

    Accessible.role: Accessible.Grouping
    Accessible.name: i18nc("@label accessible", "%1 destination", busName)

    Repeater {
        model: [
            i18nc("@action:button destination", "Phones"),
            i18nc("@action:button destination", "Stream"),
            i18nc("@action:button destination", "Both")
        ]
        QQC2.AbstractButton {
            id: segment
            required property string modelData
            required property int index
            readonly property bool selected: control.current === index

            Layout.fillWidth: true
            Layout.preferredWidth: 1
            implicitHeight: Kirigami.Units.gridUnit * 1.5
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            text: modelData

            Accessible.role: Accessible.RadioButton
            Accessible.checkable: true
            Accessible.checked: selected
            Accessible.name: i18nc("@action:button accessible", "%1 destination %2", control.busName, modelData)

            onClicked: control.picked(index)

            background: Rectangle {
                color: segment.selected ? Kirigami.Theme.highlightColor
                     : segment.down ? Qt.alpha(Kirigami.Theme.textColor, 0.15)
                     : segment.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.08)
                     : "transparent"
                border.width: segment.visualFocus ? 2 : 1
                border.color: segment.visualFocus ? Kirigami.Theme.focusColor : Qt.alpha(Kirigami.Theme.textColor, 0.25)
                topLeftRadius: segment.index === 0 ? Kirigami.Units.cornerRadius : 0
                bottomLeftRadius: segment.index === 0 ? Kirigami.Units.cornerRadius : 0
                topRightRadius: segment.index === 2 ? Kirigami.Units.cornerRadius : 0
                bottomRightRadius: segment.index === 2 ? Kirigami.Units.cornerRadius : 0
            }
            contentItem: QQC2.Label {
                text: segment.text
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                font.weight: segment.selected ? Font.Bold : Font.Normal
                color: segment.selected ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }
}
