import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Headphones / Stream / Both as one segmented control, not a buried menu. Segments carry icons
// so they fit a narrow strip; the caption below names the choice, so color is never the only
// signal.
ColumnLayout {
    id: control

    property int current: 2
    property string busName

    signal picked(int index)

    readonly property var names: [
        i18nc("@action:button destination", "Headphones"),
        i18nc("@action:button destination", "Stream"),
        i18nc("@action:button destination", "Both")
    ]
    readonly property var captions: [
        i18nc("@info destination caption", "Headphones only"),
        i18nc("@info destination caption", "Stream only"),
        i18nc("@info destination caption", "Headphones + Stream")
    ]

    spacing: 3

    Accessible.role: Accessible.Grouping
    Accessible.name: i18nc("@label accessible", "%1 destination", busName)

    RowLayout {
        Layout.fillWidth: true
        spacing: 0

        Repeater {
            model: 3
            QQC2.AbstractButton {
                id: segment
                required property int index
                readonly property bool selected: control.current === index
                readonly property color ink: selected ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor

                Layout.fillWidth: true
                Layout.preferredWidth: 1
                implicitHeight: Kirigami.Units.gridUnit * 1.5
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                text: control.names[index]

                Accessible.role: Accessible.RadioButton
                Accessible.checkable: true
                Accessible.checked: selected
                Accessible.name: i18nc("@action:button accessible", "%1 destination %2", control.busName, text)

                QQC2.ToolTip.text: control.captions[index]
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

                onClicked: control.picked(index)

                background: Rectangle {
                    color: segment.selected ? Kirigami.Theme.highlightColor
                         : segment.down ? Qt.alpha(Kirigami.Theme.textColor, 0.16)
                         : segment.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.1)
                         : Qt.alpha(Kirigami.Theme.textColor, 0.05)
                    border.width: segment.visualFocus ? 2 : 1
                    border.color: segment.visualFocus ? Kirigami.Theme.focusColor
                                : segment.selected ? Kirigami.Theme.highlightColor
                                : Qt.alpha(Kirigami.Theme.textColor, 0.18)
                    topLeftRadius: segment.index === 0 ? Kirigami.Units.cornerRadius : 0
                    bottomLeftRadius: segment.index === 0 ? Kirigami.Units.cornerRadius : 0
                    topRightRadius: segment.index === 2 ? Kirigami.Units.cornerRadius : 0
                    bottomRightRadius: segment.index === 2 ? Kirigami.Units.cornerRadius : 0
                }

                contentItem: Item {
                    Row {
                        anchors.centerIn: parent
                        spacing: 3
                        Kirigami.Icon {
                            visible: segment.index !== 1
                            anchors.verticalCenter: parent.verticalCenter
                            source: "audio-headphones"
                            isMask: true
                            color: segment.ink
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: implicitWidth
                        }
                        StreamDot {
                            visible: segment.index !== 0
                            anchors.verticalCenter: parent.verticalCenter
                            size: Kirigami.Units.iconSizes.small * 0.55
                            border.width: segment.selected ? 1 : 0
                            border.color: segment.ink
                        }
                    }
                }
            }
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        text: control.captions[control.current] ?? ""
        font: Kirigami.Theme.smallFont
        opacity: 0.65
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        Accessible.ignored: true
    }
}
