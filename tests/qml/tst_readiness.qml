import QtQuick
import QtQuick.Controls as QQC2
import QtTest
import "../../src/app/qml"
Item {
    width: 900
    height: 900
    ReadinessPanel { id: panel; width: 850 }
    SignalSpy { id: navigated; target: panel; signalName: "navigateRequested" }
    TestCase {
        name: "ReadinessPanel"
        when: windowShown
        function init() {
            panel.results = []
            panel.checking = false
            panel.detailsOpen = false
            panel.width = 850
            navigated.clear()
        }
        function test_summaryThenFailingRows() {
            // The page owns the one Check Again button; the panel only shows results.
            compare(findChild(panel, "checkReadiness"), null)
            panel.results = [
                {id: "mic-level", title: "Mic", detail: "Muted", label: "Needs attention", status: "attention"},
                {id: "mic-device", title: "Microphone", detail: "Resolved", label: "Verified", status: "verified"},
                {id: "obs", title: "OBS", detail: "Disconnected", label: "Not verified", status: "unknown"}
            ]
            tryCompare(panel, "attentionCount", 1)
            compare(findChild(panel, "readinessSummary").text, "1 check needs attention")
            // The failing row is in view with its fix; everything else waits behind Show checks.
            compare(findChild(panel, "row-mic-level").visible, true)
            compare(findChild(panel, "correct-mic-level").visible, true)
            compare(findChild(panel, "group-devices").visible, false)
            compare(findChild(panel, "group-obs").visible, false)
            const show = findChild(panel, "showChecks")
            compare(show.Accessible.name, "Show checks")
            compare(show.Accessible.description, "Collapsed")
            mouseClick(show)
            compare(show.Accessible.description, "Expanded")
            compare(findChild(panel, "group-devices").visible, true)
            compare(findChild(panel, "group-obs").visible, true)
            panel.results = [panel.results[1], panel.results[2]]
            tryCompare(findChild(panel, "readinessSummary"), "text", "Some checks could not be verified")
            panel.results = [panel.results[0]]
            tryCompare(findChild(panel, "readinessSummary"), "text", "Controls and routing checked")
        }
        function test_groupingAndScope() {
            panel.results = [
                {id: "mic-level", title: "Mic", detail: "Muted", label: "Needs attention", status: "attention"},
                {id: "phones-device", title: "Headphones", detail: "Missing", label: "Needs attention", status: "attention"},
                {id: "desktop-stream", title: "Desktop → stream", detail: "Excluded", label: "Intentionally excluded/idle", status: "excluded"},
                {id: "obs", title: "OBS", detail: "Disconnected", label: "Not verified", status: "unknown"},
                {id: "output-proof", title: "Audience audio", detail: "Not tested", label: "Not verified", status: "unknown"}
            ]
            tryCompare(panel, "attentionCount", 2)
            compare(panel.unknownCount, 1) // Permanent scope note is not an unresolved check.
            compare(panel.excludedCount, 1)
            compare(panel.groups[0].rows.length, 0)
            compare(panel.attentionRows.length, 2)
            compare(findChild(panel, "attentionRows").count, 2)
            compare(panel.groups[2].rows.length, 1)
            compare(findChild(panel, "group-controls").visible, false)
            const problem = findChild(panel, "row-mic-level")
            compare(problem.Accessible.name, "Mic, Needs attention")
            compare(problem.detailsOpen, false)
            compare(findChild(panel, "correct-mic-level").Accessible.name, "Open mixer: Mic")
            problem.forceActiveFocus()
            compare(problem.activeFocus, true)
            keyClick(Qt.Key_Tab)
            compare(findChild(panel, "correct-mic-level").activeFocus, true)
            mouseClick(findChild(panel, "correct-mic-level"))
            compare(navigated.signalArguments[0][0], "mixer")
            compare(findChild(panel, "group-routing").expanded, false)
            compare(findChild(panel, "group-obs").expanded, true)
        }
        function test_healthyDetailsAreAvailable() {
            panel.results = [{id: "mic-device", title: "Microphone", detail: "Selected: saved. Resolved: device.", label: "Verified", status: "verified"}]
            tryCompare(panel, "verifiedCount", 1)
            const group = findChild(panel, "group-devices")
            // Everything passes: only the summary line shows until asked for.
            compare(findChild(panel, "readinessSummary").text, "Controls and routing checked")
            compare(group.visible, false)
            const show = findChild(panel, "showChecks")
            compare(show.Accessible.description, "Collapsed")
            mouseClick(show)
            compare(show.Accessible.description, "Expanded")
            compare(group.visible, true)
            compare(group.expanded, false)
            const toggle = findChild(panel, "toggle-devices")
            compare(toggle.Accessible.name, "Devices")
            toggle.forceActiveFocus()
            keyClick(Qt.Key_Space)
            compare(group.expanded, true)
            compare(toggle.Accessible.checked, true)
            const row = findChild(panel, "row-mic-device")
            compare(row.detailsOpen, false)
            tryVerify(() => row.width > panel.width / 2) // laid out after the group opened
            mouseClick(row)
            compare(row.detailsOpen, true)
            compare(findChild(panel, "detail-mic-device").text, panel.results[0].detail)
            row.forceActiveFocus()
            keyClick(Qt.Key_Space)
            compare(row.detailsOpen, false)
            keyClick(Qt.Key_Space)
            compare(row.detailsOpen, true)
            panel.width = 340
            tryVerify(() => row.width <= panel.width)
            panel.results = panel.results.map(r => Object.assign({}, r))
            tryCompare(group, "expanded", true)
            compare(findChild(panel, "row-mic-device").detailsOpen, true)
        }
    }
}
