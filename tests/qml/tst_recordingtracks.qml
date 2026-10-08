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
            {track: 4, trackName: "Music", busId: ":keep-obs:", current: "Currently unused"},
            {track: 5, trackName: "Discord", busId: "voice", current: "Currently unused"},
            {track: 6, trackName: "Browser", busId: "music", current: "Currently unused"}
        ]
        property var choices: [
            {id: ":keep-obs:", label: "Keep OBS assignments"}, {id: "", label: "Unused"}, {id: "mic", label: "Mic"},
            {id: "game", label: "Game"}, {id: "voice", label: "Voice"}, {id: "music", label: "Music"},
            {id: "desktop", label: "Desktop"}
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
    SignalSpy { id: choiceSpy; target: controller; signalName: "chosen" }
    SignalSpy { id: reviewed; target: controller; signalName: "reviewed" }
    TestCase {
        name: "RecordingTracks"
        when: windowShown
        function init() {
            panel.width = 800
            findChild(panel, "recordingBus4").currentIndex = 0
            controller.canPreview = true
            controller.canUndo = false
            reviewed.clear()
            choiceSpy.clear()
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
            compare(findChild(panel, "recordingTrack3").text, "Track 3")
            compare(findChild(panel, "recordingTrack4").text, "Track 4")
            compare(findChild(panel, "recordingTrack3").obsName, "Game")
            compare(findChild(panel, "recordingTrack4").obsName, "Music")
            verify(findChild(panel, "recordingHelp"))
            verify(!panel.compact)
            compare(findChild(panel, "recordingBus3").currentValue, "mic")
            compare(findChild(panel, "recordingBus4").currentValue, ":keep-obs:")
            compare(findChild(panel, "recordingChoiceHelp4").text, "Keep sources and recording output selection.")
            compare(findChild(panel, "recordingBus5").currentValue, "voice")
            compare(findChild(panel, "recordingBus6").currentValue, "music")
            compare(findChild(panel, "undoRecording").enabled, false)
            controller.canUndo = true
            compare(findChild(panel, "undoRecording").enabled, true)
        }
        function test_savedSelectionSurvivesRefresh() {
            const oldRows = controller.rows
            const oldChoices = controller.choices
            controller.rows = [
                {track: 3, trackName: "Game", busId: "mic", current: "Mic/Aux"},
                {track: 4, trackName: "Music", busId: "music", current: "Rostrum Music (Recording) (rostrum.music.monitor)"},
                {track: 5, trackName: "Discord", busId: "voice", current: "Rostrum Voice (Recording)"},
                {track: 6, trackName: "Browser", busId: "desktop", current: "Rostrum Desktop (Recording)"}
            ]
            try {
                compare(findChild(panel, "recordingBus4").currentValue, "music")
                compare(findChild(panel, "recordingCurrent4").text,
                        "Rostrum Music (Recording) (rostrum.music.monitor)")
                controller.choices = oldChoices.map(item => ({id: item.id, label: item.label}))
                compare(findChild(panel, "recordingBus4").currentValue, "music")
            } finally {
                controller.rows = oldRows
                controller.choices = oldChoices
            }
        }
        function test_obsNameDoesNotLookLikeABusAssignment() {
            const label = findChild(panel, "recordingTrackName3")
            verify(!label || !label.visible)
        }
        function test_clearChoiceIsExplicit() {
            const choice = findChild(panel, "recordingBus4")
            choice.forceActiveFocus()
            keyClick(Qt.Key_Down)
            compare(choice.currentValue, "")
            compare(choiceSpy.count, 1)
            compare(choiceSpy.signalArguments[0][0], 4)
            compare(choiceSpy.signalArguments[0][1], "")
            compare(findChild(panel, "recordingChoiceHelp4").text, "Clear source assignments and turn off this recording track.")
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
