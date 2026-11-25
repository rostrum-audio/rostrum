import QtQuick
import QtQuick.Controls as QQC2
import QtTest
import org.kde.kirigami as Kirigami
import "../../src/app/qml"

Item {
    id: root
    width: 1280
    height: 500

    property real phonesValue: 0.5
    property real streamValue: 0.5
    property int linkPresses: 0
    property int phonesEdits: 0
    property int streamEdits: 0
    property real lastPhones: -1
    property real lastStream: -1

    MixFaders {
        id: box
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: Kirigami.Units.gridUnit * 8
        height: 360
        phonesValue: root.phonesValue
        streamValue: root.streamValue
        phonesEnabled: phonesOn
        streamEnabled: true
        onPhonesEdited: v => { root.phonesEdits++; root.lastPhones = v }
        onStreamEdited: v => { root.streamEdits++; root.lastStream = v }
        onLinkToggled: root.linkPresses++
        busName: "Sounds"
        chatShown: root.chatOn
        chatEnabled: root.chatLive
        chatMuted: !root.chatLive
        onChatMuteToggled: root.chatMutePresses++
        onChatMoved: v => { root.chatEdits++; root.lastChat = v }
    }
    property bool chatOn: false
    property bool chatLive: true
    property int chatMutePresses: 0
    property int chatEdits: 0
    property real lastChat: -1
    property bool phonesOn: true

    Flickable {
        id: flick
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 80
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        contentWidth: row.width
        contentHeight: height

        Row {
            id: row
            spacing: Kirigami.Units.smallSpacing * 2
            height: flick.height
            Repeater {
                model: 11
                MixFaders {
                    height: row.height
                }
            }
        }
    }

    TestCase {
        name: "MixFaders"
        when: windowShown

        function init() {
            root.phonesValue = 0.5
            root.streamValue = 0.5
            root.phonesOn = true
            root.linkPresses = 0
            root.phonesEdits = 0
            root.streamEdits = 0
            root.lastPhones = -1
            root.lastStream = -1
            root.chatOn = false
            root.chatLive = true
            root.chatMutePresses = 0
            root.chatEdits = 0
            root.lastChat = -1
        }

        function test_chatFaderOnlyWhileShown() {
            compare(findChild(box, "chatFader").visible, false)
            compare(findChild(box, "chatMuteButton").visible, false)
            root.chatOn = true
            compare(findChild(box, "chatFader").visible, true)
            compare(findChild(box, "chatMuteButton").visible, true)
            // the link only covers Headphones and Stream
            mouseClick(findChild(box, "linkButton"))
            compare(root.linkPresses, 1)
            compare(root.chatEdits, 0)
        }

        function test_chatFaderDisabledWhileOff() {
            root.chatOn = true
            const fader = findChild(box, "chatFader")
            compare(fader.enabled, true)
            root.chatLive = false
            compare(fader.enabled, false)
            mouseDrag(fader, fader.width / 2, fader.height / 2, 0, 80)
            compare(root.chatEdits, 0)
        }

        function test_chatFaderEditsAndResets() {
            root.chatOn = true
            const fader = findChild(box, "chatFader")
            compare(fader.resetValue, 0.7)
            fader.forceActiveFocus()
            keyClick(Qt.Key_Up)
            compare(root.chatEdits, 1)
            verify(Math.abs(root.lastChat - 0.71) < 0.001)
            compare(root.phonesEdits, 0)
            compare(root.streamEdits, 0)
        }

        function test_chatMuteButtonEmits() {
            root.chatOn = true
            const button = findChild(box, "chatMuteButton")
            compare(button.Accessible.name, "Sounds: Mute sounds in voice chat")
            mouseClick(button)
            compare(root.chatMutePresses, 1)
        }

        function test_twoFadersAndLink() {
            verify(findChild(box, "phonesFader") !== null)
            verify(findChild(box, "streamFader") !== null)
            const button = findChild(box, "linkButton")
            verify(button !== null)
            mouseClick(button)
            compare(root.linkPresses, 1)
        }

        function test_disabledMixDoesNotEdit() {
            root.phonesOn = true
            const phones = findChild(box, "phonesFader")
            wait(1)
            root.phonesOn = false
            compare(phones.enabled, false)
            mouseDrag(phones, phones.width / 2, phones.height / 2, 0, 80)
            compare(root.phonesEdits, 0)
        }

        function test_keyboardOnEachFader() {
            const stream = findChild(box, "streamFader")
            stream.forceActiveFocus()
            verify(stream.activeFocus)
            keyClick(Qt.Key_Up)
            compare(root.streamEdits, 1)
            compare(root.phonesEdits, 0)
            verify(Math.abs(root.lastStream - 0.51) < 0.001)
        }

        function test_elevenBusesScroll() {
            verify(flick.contentWidth > flick.width)
            verify(flick.interactive)
        }
    }
}
