import QtQuick
import org.kde.kirigami as Kirigami

// The on-air dot: the stream's mark everywhere in the app, in the theme's negative (red) colour.
Rectangle {
    property real size: Kirigami.Units.smallSpacing * 2.5

    implicitWidth: size
    implicitHeight: size
    radius: width / 2
    color: Kirigami.Theme.negativeTextColor
}
