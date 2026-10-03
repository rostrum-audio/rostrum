import QtQuick
import QtQuick.Controls as QQC2
import QtTest
import "../../src/app/qml"

Item {
    width: 300
    height: 400

    property real level: 0.3

    QQC2.Slider {
        id: slider
        orientation: Qt.Vertical
        width: 60
        height: 300
        from: 0
        to: 1
        stepSize: 0
        value: level
        onMoved: level = value

        SliderReset {
            target: slider
            onTriggered: level = 1.0
        }
    }

    TestCase {
        name: "SliderReset"
        when: windowShown

        function handleY() {
            return slider.topPadding + slider.visualPosition * slider.availableHeight
        }

        function init() {
            level = 0.3
            wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        }

        function test_doubleClickResets() {
            mouseDoubleClickSequence(slider, slider.width / 2, handleY())
            compare(level, 1.0)
        }

        function test_singleClickDoesNotReset() {
            mouseClick(slider, slider.width / 2, handleY())
            verify(Math.abs(level - 0.3) < 0.05)
        }

        function test_clickThenDragDoesNotReset() {
            const y = handleY()
            mouseClick(slider, slider.width / 2, y)
            mousePress(slider, slider.width / 2, y)
            mouseMove(slider, slider.width / 2, y + 40)
            mouseMove(slider, slider.width / 2, y + 90)
            mouseRelease(slider, slider.width / 2, y + 90)
            verify(level < 0.25)
        }
    }
}
