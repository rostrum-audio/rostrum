import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Rostrum

Kirigami.Page {
    id: shell

    readonly property var pages: [
        { id: "mixer", label: i18nc("@title page", "Mixer"), icon: "view-media-equalizer" },
        { id: "apps", label: i18nc("@title page", "Apps"), icon: "applications-multimedia" },
        { id: "scenes", label: i18nc("@title page", "Scenes"), icon: "view-media-playlist" },
        { id: "devices", label: i18nc("@title page", "Devices"), icon: "audio-card" },
        { id: "obs", label: i18nc("@title page", "OBS"), icon: "media-record" },
        { id: "settings", label: i18nc("@title page", "Settings"), icon: "settings-configure" }
    ]
    readonly property int pageIndex: Math.max(0, pages.findIndex(p => p.id === App.lastPage))
    readonly property bool blocked: App.pipewireState === "missing" || App.pipewireState === "connecting"
    readonly property bool inWizard: !App.wizardDone && !blocked
    readonly property bool sidebarHasFocus: sidebar.activeFocusInside

    function focusSidebar() {
        sidebar.focusList()
    }
    function focusPage() {
        const item = stack.children[stack.currentIndex]
        if (item) {
            item.forceActiveFocus(Qt.TabFocusReason)
            item.nextItemInFocusChain(true)?.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    padding: 0
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            id: sidebar
            visible: !shell.inWizard
            Layout.fillHeight: true
            pages: shell.pages
            currentIndex: shell.pageIndex
            enabled: !shell.blocked
            onPageRequested: id => App.lastPage = id
        }

        Kirigami.Separator {
            visible: !shell.inWizard
            Layout.fillHeight: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Banners {
                visible: !shell.blocked
                Layout.fillWidth: true
            }

            Wizard {
                visible: shell.inWizard
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            StackLayout {
                id: stack
                visible: !shell.blocked && !shell.inWizard
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: shell.pageIndex

                MixerPage {}
                AppsPage {}
                ScenesPage {}
                DevicesPage {}
                ObsPage {}
                SettingsPage {}
            }

            PipeWireMissingPage {
                visible: App.pipewireState === "missing"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            Item {
                visible: App.pipewireState === "connecting"
                Layout.fillWidth: true
                Layout.fillHeight: true
                QQC2.BusyIndicator {
                    anchors.centerIn: parent
                    running: parent.visible
                    Accessible.name: i18n("Connecting to PipeWire")
                }
            }
        }
    }
}
