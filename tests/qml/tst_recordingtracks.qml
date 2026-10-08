import QtQuick
import QtTest
import "../../src/app/qml"

Item {
    width: 900
    height: 1100
    QtObject {
        id: controller
        property var rows: [
            {track: 3, trackName: "Game", busId: "mic", current: "Mic/Aux"},
            {track: 4, trackName: "Music", busId: "game", current: "Currently unused"},
            {track: 5, trackName: "Discord", busId: "voice", current: "Currently unused"},
            {track: 6, trackName: "Browser", busId: "music", current: "Currently unused"}
        ]
        property var choices: [
            {id: "", label: "Unused"}, {id: "mic", label: "Mic"},
            {id: "game", label: "Game"}, {id: "voice", label: "Voice"}, {id: "music", label: "Music"}
        ]
        property string collection: "Test"
        property string status: "Review the changes"
        property var previewItems: ["Mic/Aux: tracks 1, 2 → 1, 2, 3"]
        property bool canPreview: true
        property bool canApply: true
        property bool canUndo: false
        signal previewReady()
        signal chosen(int track, string bus)
        signal reviewed()
        signal applied()
        signal undone()
        function choose(track, bus) { chosen(track, bus) }
        function preview() { controller.reviewed() }
        function apply() { applied() }
        function undo() { undone() }
    }
    RecordingTracks { id: panel; width: 800; setup: controller }
    SignalSpy { id: reviewed; target: controller; signalName: "reviewed" }
    TestCase {
        name: "RecordingTracks"
        when: windowShown
        function init() {
            panel.width = 800
            controller.canPreview = true
            controller.canUndo = false
            reviewed.clear()
        }
        function test_reviewIsExplicit() {
            const button = findChild(panel, "reviewRecording")
            verify(button)
            mouseClick(button)
            compare(reviewed.count, 1)
            controller.canPreview = false
            compare(button.enabled, false)
            compare(findChild(panel, "recordingBus3").enabled, false)
        }
        function test_labelsAndSelections() {
            compare(findChild(panel, "recordingTrackName3").text, "Game")
            compare(findChild(panel, "recordingTrackName4").text, "Music")
            verify(findChild(panel, "recordingHelp"))
            verify(!panel.compact)
            compare(findChild(panel, "recordingBus3").currentValue, "mic")
            compare(findChild(panel, "recordingBus4").currentValue, "game")
            compare(findChild(panel, "recordingBus5").currentValue, "voice")
            compare(findChild(panel, "recordingBus6").currentValue, "music")
            compare(findChild(panel, "undoRecording").enabled, false)
            controller.canUndo = true
            compare(findChild(panel, "undoRecording").enabled, true)
        }
        function test_narrowLayout() {
            panel.width = 430
            wait(10)
            verify(panel.compact)
            const choice = findChild(panel, "recordingBus4")
            verify(choice.width > 150)
            verify(choice.mapToItem(panel, choice.width, 0).x <= panel.width + 1)
            const undo = findChild(panel, "undoRecording")
            verify(undo.mapToItem(panel, undo.width, 0).x <= panel.width + 1)
        }
    }
}
