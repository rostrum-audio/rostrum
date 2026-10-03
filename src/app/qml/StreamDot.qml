import QtQuick

// The on-air dot: the stream's mark everywhere in the app, in the logo's coral.
Rectangle {
    property real size: 10

    implicitWidth: size
    implicitHeight: size
    radius: width / 2
    color: "#FF4F5E"
}
