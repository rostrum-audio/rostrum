pragma Singleton
import QtQuick

// RuleRow.qml saves an edited match through Apps.editMatch. The quick tests record the call.
QtObject {
    property var lastEdit: []

    function editMatch(key, match, matchKey) {
        lastEdit = [key, match, matchKey]
        return true
    }
}
