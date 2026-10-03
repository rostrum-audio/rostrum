import QtQuick
import org.kde.kirigami as Kirigami

// Peak meter. The bus color runs to −12 dB, amber to −6 dB, red above. A clip mark holds for
// 1.5 s (timed by the Mixer). The level is also spoken, at most twice a second.
Item {
    id: meter

    property real value: 0 // 0..1, −60..0 dB
    property bool clip: false
    property color color: Kirigami.Theme.positiveTextColor
    property int orientation: Qt.Vertical
    property string accessibleName

    readonly property bool vertical: orientation === Qt.Vertical
    readonly property real amberAt: 0.8 // −12 dB on a 60 dB scale
    readonly property real redAt: 0.9   // −6 dB
    readonly property real thickness: Kirigami.Units.smallSpacing * 2
    readonly property real clipSize: Kirigami.Units.smallSpacing * 1.5
    property string spokenLevel: i18nc("@info meter level", "Silent")

    implicitWidth: vertical ? thickness : Kirigami.Units.gridUnit * 6
    implicitHeight: vertical ? Kirigami.Units.gridUnit * 6 : thickness

    Accessible.role: Accessible.ProgressBar
    Accessible.name: accessibleName
    Accessible.description: spokenLevel

    Timer {
        interval: 500
        running: meter.visible
        repeat: true
        onTriggered: {
            const text = meter.value <= 0.001 ? i18nc("@info meter level", "Silent")
                       : meter.clip ? i18nc("@info meter level", "Clipping")
                       : i18nc("@info meter level in dB", "%1 dB", Math.round(meter.value * 60 - 60))
            if (text !== meter.spokenLevel) {
                meter.spokenLevel = text
            }
        }
    }

    // Track
    Item {
        id: track
        x: 0
        y: meter.vertical ? meter.clipSize + 1 : 0
        width: meter.vertical ? meter.width : meter.width - meter.clipSize - 1
        height: meter.vertical ? meter.height - meter.clipSize - 1 : meter.height

        Rectangle {
            anchors.fill: parent
            radius: 2
            color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
        }

        // Lit part: the zones are laid out over the full track and clipped to the level.
        Item {
            clip: true
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: meter.vertical ? parent.width : parent.width * Math.min(1, meter.value)
            height: meter.vertical ? parent.height * Math.min(1, meter.value) : parent.height

            Repeater {
                model: [
                    { from: 0, to: meter.amberAt, color: meter.color },
                    { from: meter.amberAt, to: meter.redAt, color: Kirigami.Theme.neutralTextColor },
                    { from: meter.redAt, to: 1, color: Kirigami.Theme.negativeTextColor }
                ]
                Rectangle {
                    required property var modelData
                    color: modelData.color
                    x: meter.vertical ? 0 : track.width * modelData.from
                    width: meter.vertical ? track.width : track.width * (modelData.to - modelData.from)
                    height: meter.vertical ? track.height * (modelData.to - modelData.from) : track.height
                    // Anchored to the bottom of the clipped item, which grows upwards.
                    y: meter.vertical ? parent.height - track.height * modelData.to : 0
                }
            }
        }
    }

    // Clip indicator: top end for vertical meters, right end for horizontal ones.
    Rectangle {
        x: meter.vertical ? 0 : meter.width - meter.clipSize
        y: 0
        width: meter.vertical ? meter.width : meter.clipSize
        height: meter.vertical ? meter.clipSize : meter.height
        radius: 1
        color: meter.clip ? Kirigami.Theme.negativeTextColor : Qt.alpha(Kirigami.Theme.textColor, 0.12)
    }
}
