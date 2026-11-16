import QtQuick
import QtTest
import "../../src/app/qml"

Item {
    id: root
    width: 900
    height: 1400
    QtObject {
        id: prefs
        property var hotkeys: [
            {id: "mic-mute", group: "mic", label: "Mute mic", description: "Mutes or unmutes the mic.", shortcut: "Meta+Alt+M", defaultShortcut: "Meta+Alt+M", problem: ""},
            {id: "push-to-talk", group: "mic", label: "Push to talk", description: "Hold to talk while the mic is muted.", shortcut: "", defaultShortcut: "", problem: ""},
            {id: "stream-mute", group: "stream", label: "Mute all playback to stream", description: "Mutes or unmutes the Stream master.", shortcut: "Meta+Alt+S", defaultShortcut: "Meta+Alt+S", problem: ""},
            {id: "scene-next", group: "scenes", label: "Next scene", description: "Loads the next scene.", shortcut: "", defaultShortcut: "", problem: ""},
            {id: "bus-game-mute", group: "buses", label: "Mute Game", description: "Mutes or unmutes Game.", shortcut: "", defaultShortcut: "", problem: ""}
        ]
        property var conflicts: ({})
        property var resets: []
        function hotkeyConflict(id, shortcut) { return conflicts[id] ?? "" }
        function setHotkeySequence(id, sequence) {}
        function setHotkey(id, shortcut) {}
        function resetHotkey(id) { resets = resets.concat([id]) }
    }
    Component { id: fresh; HotkeyList { width: 850; preferences: prefs; backend: "kglobalaccel" } }

    TestCase {
        name: "HotkeyList"
        when: windowShown
        property var list: null
        function init() {
            prefs.conflicts = ({})
            prefs.resets = []
            list = createTemporaryObject(fresh, root)
            verify(list)
        }
        function row(id) { return findChild(list, "hotkeyRow-" + id) }
        function test_groupsAndDefaults() {
            // Mic and Stream open; Scenes and Buses wait behind their titles.
            verify(row("mic-mute"))
            verify(row("stream-mute"))
            compare(row("scene-next"), null)
            compare(row("bus-game-mute"), null)
            const toggle = findChild(list, "hotkeyGroupToggle-scenes")
            compare(toggle.Accessible.name, "Scenes")
            compare(toggle.Accessible.description, "Collapsed")
            mouseClick(toggle)
            compare(toggle.Accessible.description, "Expanded")
            tryVerify(() => row("scene-next") !== null)
            mouseClick(toggle)
            tryCompare(toggle.Accessible, "description", "Collapsed")
            compare(row("scene-next"), null)
        }
        function test_searchFiltersAndRestores() {
            const search = findChild(list, "hotkeySearch")
            compare(search.Accessible.name, "Search hotkeys")
            search.text = "scene"
            // A match in a closed group shows; groups without a match go away.
            tryVerify(() => row("scene-next") !== null)
            compare(row("mic-mute"), null)
            compare(findChild(list, "hotkeyGroup-mic").visible, false)
            compare(findChild(list, "hotkeyGroup-scenes").visible, true)
            search.text = "meta+alt+s" // the shortcut itself is searchable
            tryVerify(() => row("stream-mute") !== null)
            compare(row("scene-next"), null)
            search.text = "nothing like this"
            tryCompare(list, "matches", 0)
            compare(findChild(list, "hotkeyNoMatches").visible, true)
            search.text = ""
            // Clearing the field puts every group back as it was.
            tryVerify(() => row("mic-mute") !== null)
            compare(row("scene-next"), null)
            compare(findChild(list, "hotkeyNoMatches").visible, false)
            compare(prefs.resets.length, 0) // searching never changes a shortcut
        }
        function test_conflictOpensItsGroup() {
            prefs.conflicts = {"bus-game-mute": "Mute mic"}
            const other = createTemporaryObject(fresh, root)
            tryVerify(() => findChild(other, "hotkeyRow-bus-game-mute") !== null)
            compare(findChild(other, "hotkeyRow-scene-next"), null)
        }
    }
}
