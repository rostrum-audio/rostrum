import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Rostrum

// "Send to bus" menu: every playback bus, plus Unassign when the app has one.
QQC2.Menu {
    id: menu

    property string currentBus
    property bool allowUnassign: currentBus !== ""

    signal chosen(string busId)
    signal unassignRequested()

    Instantiator {
        model: Apps.buses
        delegate: QQC2.MenuItem {
            required property var modelData
            text: modelData.name
            checkable: true
            checked: modelData.id === menu.currentBus
            icon.color: modelData.color
            icon.name: "media-record"
            onTriggered: menu.chosen(modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }

    QQC2.MenuSeparator {
        visible: menu.allowUnassign
    }
    QQC2.MenuItem {
        visible: menu.allowUnassign
        height: visible ? implicitHeight : 0
        text: i18nc("@action:inmenu", "Unassign")
        icon.name: "edit-clear"
        onTriggered: menu.unassignRequested()
    }
}
