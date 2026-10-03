import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

// M and S on a strip. Lit in its own color when on; the parent owns the state, so a click only
// asks for the change.
QQC2.AbstractButton {
    id: toggle

    property bool on: false
    property color activeColor: Kirigami.Theme.highlightColor

    implicitHeight: Kirigami.Units.gridUnit * 1.4
    implicitWidth: Kirigami.Units.gridUnit * 2
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: on

    QQC2.ToolTip.visible: hovered
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

    background: Rectangle {
        radius: Kirigami.Units.cornerRadius
        color: toggle.on ? toggle.activeColor
             : toggle.down ? Qt.alpha(Kirigami.Theme.textColor, 0.16)
             : toggle.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.1)
             : Qt.alpha(Kirigami.Theme.textColor, 0.05)
        border.width: toggle.visualFocus ? 2 : 1
        border.color: toggle.visualFocus ? Kirigami.Theme.focusColor
                    : toggle.on ? toggle.activeColor
                    : Qt.alpha(Kirigami.Theme.textColor, 0.18)
    }

    contentItem: QQC2.Label {
        text: toggle.text
        font.weight: Font.Bold
        color: toggle.on ? "white" : Qt.alpha(Kirigami.Theme.textColor, 0.8)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
