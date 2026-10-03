import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A picture-less diagram: apps go to buses, buses go to headphones and to OBS.
RowLayout {
    id: diagram

    spacing: Kirigami.Units.largeSpacing
    Accessible.role: Accessible.StaticText
    Accessible.name: i18n("Apps go to buses. Buses go to your headphones and to OBS.")

    component Box: QQC2.Label {
        property color accent: Kirigami.Theme.highlightColor
        horizontalAlignment: Text.AlignHCenter
        padding: Kirigami.Units.largeSpacing
        font.weight: Font.DemiBold
        Accessible.ignored: true
        background: Rectangle {
            radius: Kirigami.Units.cornerRadius
            color: Qt.alpha(parent.accent, 0.15)
            border.width: 1
            border.color: parent.accent
        }
    }
    component Arrow: Kirigami.Icon {
        source: "arrow-right"
        implicitWidth: Kirigami.Units.iconSizes.smallMedium
        implicitHeight: implicitWidth
        Accessible.ignored: true
    }

    Box {
        text: i18nc("diagram box", "Apps\nDiscord, game, music")
        accent: Kirigami.Theme.disabledTextColor
    }
    Arrow {}
    Box {
        text: i18nc("diagram box", "Buses\nGame, Voice, Music…")
    }
    Arrow {}
    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing
        Box {
            Layout.fillWidth: true
            text: i18nc("diagram box", "Your headphones")
            accent: Kirigami.Theme.positiveTextColor
        }
        Box {
            Layout.fillWidth: true
            text: i18nc("diagram box", "OBS (your stream)")
            accent: Kirigami.Theme.negativeTextColor
        }
    }
}
