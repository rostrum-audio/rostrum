import QtQuick
import QtQuick.Shapes
import org.kde.kirigami as Kirigami

// Drawn, not an icon: theme arrows blur and misalign at small sizes on fractional scales.
// The Shape sits inside a plain Item so its path cannot feed back into its own size.
Item {
    id: chevron

    property color color: Kirigami.Theme.textColor
    property bool up: false
    property real lineWidth: 1.5

    implicitWidth: 10
    implicitHeight: 6

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            id: path
            readonly property real inset: chevron.lineWidth / 2
            readonly property real top: chevron.up ? chevron.height - inset : inset
            readonly property real tip: chevron.up ? inset : chevron.height - inset

            strokeColor: chevron.color
            strokeWidth: chevron.lineWidth
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: inset
            startY: top
            PathLine {
                x: chevron.width / 2
                y: path.tip
            }
            PathLine {
                x: chevron.width - path.inset
                y: path.top
            }
        }
    }
}
