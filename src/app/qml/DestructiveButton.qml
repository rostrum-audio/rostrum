import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A button for the choice that removes or overwrites something. It is drawn in the theme's
// negative colours and never takes the default focus; the icon and wording say what it does, so
// the meaning does not rest on colour.
QQC2.AbstractButton {
    id: button

    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    leftPadding: Kirigami.Units.largeSpacing
    rightPadding: Kirigami.Units.largeSpacing
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing
    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, Kirigami.Units.gridUnit * 4)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Kirigami.Units.gridUnit * 1.8)
    opacity: enabled ? 1 : 0.5

    Accessible.role: Accessible.Button
    Accessible.name: text
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius
        color: button.down ? Qt.tint(Kirigami.Theme.negativeBackgroundColor, Qt.alpha(Kirigami.Theme.negativeTextColor, 0.35))
             : button.hovered ? Qt.tint(Kirigami.Theme.negativeBackgroundColor, Qt.alpha(Kirigami.Theme.negativeTextColor, 0.2))
             : Kirigami.Theme.negativeBackgroundColor
        border.width: button.visualFocus ? 2 : 1
        border.color: button.visualFocus ? Kirigami.Theme.focusColor : Kirigami.Theme.negativeTextColor
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Item {
            Layout.fillWidth: true
        }
        Kirigami.Icon {
            visible: button.icon.name.length > 0
            source: button.icon.name
            isMask: true
            color: Kirigami.Theme.negativeTextColor
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
        }
        QQC2.Label {
            text: button.text
            color: Kirigami.Theme.textColor
            font.weight: Font.DemiBold
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
