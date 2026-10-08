import QtQuick
import QtTest
import RostrumTest 1.0
import "../../src/app/qml"

Item {
    width: 900
    height: 900
    AppRowsFixture { id: rows }
    PlayingApps {
        id: view
        width: 700
        height: 700
        sourceModel: rows
        appDelegate: Component {
            Item {
                required property var row
                objectName: "app_" + row.key
                width: view.availableWidth
                height: 50
            }
        }
    }
    TestCase {
        name: "PlayingApps"
        when: windowShown
        function data(assigned) {
            return [
                {key: "firefox", name: "Firefox", binary: "firefox-bin", busId: "desktop", tool: false},
                {key: "obs", name: "OBS", binary: "obs", busId: assigned ? "desktop" : "", tool: true},
                {key: "speech", name: "speech-dispatcher-dummy", binary: "sd_dummy", busId: "", tool: true},
                {key: "own", name: "App with its own output", binary: "player", busId: "", tool: false}
            ]
        }
        function init() {
            view.width = 700
            view.query = ""
            view.busFilter = ""
            view.toolsExpanded = false
            rows.replaceRows(data(false))
            tryCompare(view, "playingCount", 2)
            tryCompare(view, "toolsCount", 2)
            const list = findChild(view, "playingAppsList")
            list.positionViewAtBeginning()
            list.forceLayout()
            wait(20)
        }
        function test_toolsStartCollapsed() {
            verify(!view.toolsVisible)
            mouseClick(findChild(view, "audioToolsToggle"))
            verify(view.toolsVisible)
            mouseClick(findChild(view, "audioToolsToggle"))
            verify(!view.toolsVisible)
        }
        function test_searchRevealsToolsInResultsAndRestoresCollapsedState() {
            view.query = " OBS "
            tryCompare(view, "playingCount", 1)
            tryCompare(view, "toolsCount", 0)
            tryVerify(() => findChild(view, "playingAppsList").itemAtIndex(0)?.row.key === "obs")
            view.query = "sd_dummy"
            tryCompare(view, "playingCount", 1)
            tryCompare(view, "toolsCount", 0)
            tryVerify(() => findChild(view, "playingAppsList").itemAtIndex(0)?.row.key === "speech")
            view.query = ""
            tryCompare(view, "playingCount", 2)
            tryCompare(view, "toolsCount", 2)
            verify(!view.toolsVisible)
        }
        function test_searchPreservesExpandedChoice() {
            view.toolsExpanded = true
            view.query = "obs"
            tryCompare(view, "toolsCount", 0)
            view.query = ""
            tryCompare(view, "toolsCount", 2)
            verify(view.toolsVisible)
        }
        function test_keyboardExpansion() {
            const toggle = findChild(view, "audioToolsToggle")
            toggle.forceActiveFocus()
            keyClick(Qt.Key_Space)
            verify(view.toolsVisible)
            keyClick(Qt.Key_Space)
            verify(!view.toolsVisible)
        }
        function test_assignmentMovesToolToMainListWithoutResettingRows() {
            rows.replaceRows(data(true)) // same keys: RowsModel emits dataChanged only
            tryCompare(view, "playingCount", 3)
            tryCompare(view, "toolsCount", 1)
            rows.replaceRows(data(false))
            tryCompare(view, "playingCount", 2)
            tryCompare(view, "toolsCount", 2)
        }
        function test_busFilterHidesUnassignedTools() {
            view.busFilter = "desktop"
            tryCompare(view, "playingCount", 1)
            tryCompare(view, "toolsCount", 0)
            rows.replaceRows(data(true))
            tryCompare(view, "playingCount", 2)
            tryCompare(view, "toolsCount", 0)
        }
        function test_lastToolStoppingRemovesSection() {
            rows.replaceRows(data(false).filter(a => !a.tool))
            tryCompare(view, "toolsCount", 0)
            verify(!findChild(view, "audioToolsToggle").visible)
        }
        function test_narrowLayout() {
            view.width = 330
            view.toolsExpanded = true
            wait(10)
            const button = findChild(view, "audioToolsToggle")
            verify(button.width <= view.availableWidth)
            verify(button.mapToItem(view, button.width, 0).x <= view.width)
        }
    }
}
