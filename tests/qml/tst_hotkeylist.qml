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
        // Clashes a newly pressed shortcut would have, by row id.
        property var pendingConflicts: ({})
        property var sets: []
        property var replaced: []
        // By row id, or by "id shortcut" for a clash only that shortcut would have.
        function hotkeyConflict(id, shortcut) { return conflicts[id + " " + shortcut] ?? conflicts[id] ?? "" }
        function hotkeySequenceConflict(id, sequence) { return pendingConflicts[id] ?? "" }
        function setHotkeySequence(id, sequence) { sets = sets.concat([id]) }
        function replaceHotkeySequence(id, sequence) { replaced = replaced.concat([id]) }
        function setHotkey(id, shortcut) {}
        function resetHotkey(id) { resets = resets.concat([id]) }
    }
    Component { id: fresh; HotkeyList { width: 850; preferences: prefs; backend: "kglobalaccel" } }

    TestCase {
        name: "HotkeyList"
        when: windowShown
        property var list: null
        property var savedHotkeys: null
        function initTestCase() { savedHotkeys = prefs.hotkeys }
        // Some tests change the rows; a failure part way must not leave that for the next test.
        function cleanup() { prefs.hotkeys = savedHotkeys }
        function init() {
            prefs.conflicts = ({})
            prefs.pendingConflicts = ({})
            prefs.resets = []
            prefs.sets = []
            prefs.replaced = []
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
            // A clash that is already saved offers Replace, and has nothing to cancel back to.
            const replace = findChild(other, "hotkeyReplace-bus-game-mute")
            verify(replace.visible)
            compare(findChild(other, "hotkeyCancel-bus-game-mute").visible, false)
            mouseClick(replace)
            compare(prefs.replaced, ["bus-game-mute"])
        }
        function test_resetToATakenDefaultAsksFirst() {
            // Mute mic gave its default away earlier; Push to talk has it now.
            prefs.hotkeys = savedHotkeys.map(r => r.id === "mic-mute" ? Object.assign({}, r, {shortcut: ""}) : r)
            prefs.conflicts = {"mic-mute Meta+Alt+M": "Push to talk"}
            prefs.pendingConflicts = {"mic-mute": "Push to talk"}
            const other = createTemporaryObject(fresh, root)
            tryVerify(() => findChild(other, "hotkeyReset-mic-mute") !== null)
            const reset = findChild(other, "hotkeyReset-mic-mute")
            verify(reset.enabled)
            compare(findChild(other, "hotkeyReplace-mic-mute").visible, false)
            reset.clicked()
            // Nothing changes until the person chooses.
            compare(prefs.resets.length, 0)
            compare(findChild(other, "hotkeyRow-mic-mute").pending, true)
            const replace = findChild(other, "hotkeyReplace-mic-mute")
            tryCompare(replace, "visible", true)
            compare(findChild(other, "hotkeyCancel-mic-mute").visible, true)
            replace.clicked()
            compare(prefs.replaced, ["mic-mute"])
            compare(prefs.resets.length, 0)
        }
        function test_resetToAFreeDefault() {
            prefs.hotkeys = savedHotkeys.map(r => r.id === "mic-mute" ? Object.assign({}, r, {shortcut: "Meta+Alt+K"}) : r)
            const other = createTemporaryObject(fresh, root)
            tryVerify(() => findChild(other, "hotkeyReset-mic-mute") !== null)
            findChild(other, "hotkeyReset-mic-mute").clicked()
            compare(prefs.resets, ["mic-mute"])
            compare(findChild(other, "hotkeyRow-mic-mute").pending, false)
        }
        function press(id, sequence) {
            const keys = findChild(list, "hotkeyKeys-" + id)
            keys.keySequence = sequence
            keys.keySequenceModified()
            return keys
        }
        function test_freeShortcutIsSetAtOnce() {
            press("mic-mute", "Meta+Alt+K")
            compare(prefs.sets, ["mic-mute"])
            compare(findChild(list, "hotkeyReplace-mic-mute").visible, false)
            compare(findChild(list, "hotkeyCancel-mic-mute").visible, false)
        }
        function test_clashWaitsForReplace() {
            prefs.pendingConflicts = {"mic-mute": "Mute all playback to stream"}
            press("mic-mute", "Meta+Alt+S")
            // Nothing is saved until the person chooses.
            compare(prefs.sets.length, 0)
            compare(prefs.replaced.length, 0)
            const replace = findChild(list, "hotkeyReplace-mic-mute")
            tryCompare(replace, "visible", true)
            compare(replace.text, "Replace")
            compare(replace.Accessible.name, "Replace: remove the shortcut from Mute all playback to stream and use it for Mute mic")
            const cancel = findChild(list, "hotkeyCancel-mic-mute")
            compare(cancel.visible, true)
            compare(cancel.Accessible.name, "Cancel: keep the old shortcut for Mute mic")
            waitForRendering(list) // the row grows to fit the buttons
            mouseClick(replace)
            compare(prefs.replaced, ["mic-mute"])
            compare(prefs.sets.length, 0)
            compare(findChild(list, "hotkeyRow-mic-mute").pending, false)
        }
        function test_clashCancelKeepsTheOldShortcut() {
            prefs.pendingConflicts = {"mic-mute": "Mute all playback to stream"}
            const keys = press("mic-mute", "Meta+Alt+S")
            const cancel = findChild(list, "hotkeyCancel-mic-mute")
            tryCompare(cancel, "visible", true)
            waitForRendering(list)
            mouseClick(cancel)
            prefs.pendingConflicts = ({})
            compare(prefs.sets.length, 0)
            compare(prefs.replaced.length, 0)
            compare(findChild(list, "hotkeyRow-mic-mute").pending, false)
            tryCompare(findChild(list, "hotkeyReplace-mic-mute"), "visible", false)
            // The field shows the saved shortcut again; pressing it once more sets nothing new.
            press("mic-mute", "Meta+Alt+M")
            compare(prefs.sets, ["mic-mute"])
            verify(keys)
        }
    }
}
