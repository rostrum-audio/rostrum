import QtQuick
import QtTest
import Rostrum
import "../../src/app/qml"

Item {
    width: 700
    height: 200

    Component {
        id: ruleRow
        RuleRow {
            width: 600
        }
    }

    TestCase {
        name: "RuleRow"
        when: windowShown

        function editKeepsKey(matchKey, index) {
            const row = createTemporaryObject(ruleRow, parent, {
                rule: {key: matchKey + ":RuneLite", match: "RuneLite", matchKey: matchKey, busName: "Game",
                       lastSeen: "now", label: ""}
            })
            verify(row)
            row.startEdit()
            const keyBox = findChild(row, "ruleKeyBox")
            verify(keyBox)
            compare(keyBox.currentIndex, index)
            row.commitEdit()
            compare(Apps.lastEdit[0], matchKey + ":RuneLite")
            compare(Apps.lastEdit[2], matchKey)
        }

        function test_javaRuleStaysJavaWhenEdited() {
            editKeepsKey("java", 2)
        }

        function test_binaryRuleStaysBinaryWhenEdited() {
            editKeepsKey("binary", 1)
        }

        function test_nameRuleStaysNameWhenEdited() {
            editKeepsKey("name", 0)
        }
    }
}
