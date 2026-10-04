import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: panel
    property var results: []
    property bool checking: false
    property var openEvidence: ({})
    property bool scopeOpen: false
    signal checkRequested
    signal navigateRequested(string page)
    signal reviewObsRequested
    spacing: Kirigami.Units.largeSpacing

    readonly property var checks: results.filter(r => r.id !== "output-proof")
    readonly property int attentionCount: checks.filter(r => r.status === "attention").length
    readonly property int unknownCount: checks.filter(r => r.status === "unknown").length
    readonly property int verifiedCount: checks.filter(r => r.status === "verified").length
    readonly property int excludedCount: checks.filter(r => r.status === "excluded").length
    readonly property var definitions: [
        {
            key: "controls",
            title: qsTr("Controls"),
            icon: "audio-volume-high"
        },
        {
            key: "devices",
            title: qsTr("Devices"),
            icon: "audio-card"
        },
        {
            key: "routing",
            title: qsTr("Routing"),
            icon: "network-connect"
        },
        {
            key: "obs",
            title: qsTr("OBS capture"),
            icon: "media-record"
        }
    ]
    function category(id) {
        if (id.startsWith("obs"))
            return "obs";
        if (id.endsWith("-device"))
            return "devices";
        if (id.endsWith("-level"))
            return "controls";
        return "routing";
    }
    readonly property var attentionRows: checks.filter(r => r.status === "attention")
    readonly property var groups: definitions.map(d => ({
                key: d.key,
                rows: checks.filter(r => r.status !== "attention" && category(r.id) === d.key)
            }))
    function explanation(id) {
        if (id === "mic-level")
            return qsTr("Review the microphone's effective mute, gain and stream destination.");
        if (id === "stream-level")
            return qsTr("The Stream Mix is effectively silent. Review mute, gain and solo controls.");
        if (id.endsWith("-device"))
            return qsTr("The selected device is missing or could not be resolved. Review the selection and fallback settings.");
        if (id.endsWith("-observed-level"))
            return qsTr("Observed audio properties indicate silence where audio is expected.");
        if (id.startsWith("obs"))
            return qsTr("Review the OBS capture target, mute and track assignments. Expand this result for the observed issue.");
        return qsTr("Observed routing does not match the intended path. Expand this result for the missing or unexpected link.");
    }
    function correction(id) {
        if (id.startsWith("obs"))
            return {
                page: "obs",
                label: qsTr("Review OBS setup")
            };
        if (id.startsWith("app-"))
            return {
                page: "apps",
                label: qsTr("Review app routing")
            };
        if (id.endsWith("-device") || id === "mic-route" || id === "phones-output")
            return {
                page: "devices",
                label: qsTr("Open devices")
            };
        if (id === "mic-filters")
            return {
                page: "filters",
                label: qsTr("Open mic filters")
            };
        return {
            page: "mixer",
            label: qsTr("Open mixer")
        };
    }
    function statusColor(status) {
        if (status === "attention")
            return Kirigami.Theme.neutralTextColor;
        if (status === "verified")
            return Kirigami.Theme.positiveTextColor;
        return Kirigami.Theme.textColor;
    }
    function statusIcon(status) {
        if (status === "attention")
            return "dialog-warning";
        if (status === "verified")
            return "checkmark";
        if (status === "excluded")
            return "media-playback-pause";
        return "help-about";
    }
    function toggleEvidence(id, defaultOpen) {
        const next = Object.assign({}, openEvidence);
        next[id] = !(next[id] === undefined ? defaultOpen : next[id]);
        openEvidence = next;
    }

    component ResultEvidence: ColumnLayout {
        required property var modelData
        Layout.fillWidth: true
        spacing: 0
        Kirigami.Separator {
            Layout.fillWidth: true
        }
        QQC2.ItemDelegate {
            id: resultRow
            focusPolicy: Qt.StrongFocus
            objectName: "row-" + modelData.id
            readonly property bool detailsOpen: panel.openEvidence[modelData.id] === true
            Layout.fillWidth: true
            onClicked: panel.toggleEvidence(modelData.id, false)
            Accessible.name: modelData.title + ", " + modelData.label
            Accessible.description: detailsOpen ? qsTr("Collapse observed evidence") : qsTr("Expand observed evidence")
            Accessible.checkable: true
            Accessible.checked: detailsOpen
            contentItem: GridLayout {
                columns: panel.width < Kirigami.Units.gridUnit * 32 ? 1 : 2
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing
                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.Icon {
                        source: panel.statusIcon(modelData.status)
                        color: panel.statusColor(modelData.status)
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: implicitWidth
                        Accessible.ignored: true
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: modelData.title
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                    }
                    Kirigami.Icon {
                        source: resultRow.detailsOpen ? "arrow-down" : "arrow-right"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: implicitWidth
                        Accessible.ignored: true
                    }
                }
                StatusBadge {
                    Layout.alignment: Qt.AlignRight
                    status: modelData.status
                    label: modelData.label
                }
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            visible: modelData.status === "attention"
            text: modelData.detail.length <= 180 ? modelData.detail : panel.explanation(modelData.id)
            wrapMode: Text.WordWrap
        }
        QQC2.Button {
            objectName: "correct-" + modelData.id
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.smallSpacing
            visible: modelData.status === "attention"
            text: panel.correction(modelData.id).label
            icon.name: "go-next"
            Accessible.name: text + ": " + modelData.title
            onClicked: {
                const action = panel.correction(modelData.id);
                if (action.page === "obs")
                    panel.reviewObsRequested();
                else
                    panel.navigateRequested(action.page);
            }
        }
        QQC2.Label {
            objectName: "detail-" + modelData.id
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.topMargin: 0
            visible: resultRow.detailsOpen
            text: modelData.detail
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: Kirigami.Theme.textColor
        }
    }

    component StatusBadge: Rectangle {
        id: badge
        property string status
        property string label
        readonly property color ink: panel.statusColor(status)
        implicitWidth: badgeText.implicitWidth + Kirigami.Units.largeSpacing * 2
        implicitHeight: badgeText.implicitHeight + Kirigami.Units.smallSpacing * 2
        radius: Kirigami.Units.smallSpacing
        color: Qt.rgba(ink.r, ink.g, ink.b, 0.10)
        QQC2.Label {
            id: badgeText
            anchors.centerIn: parent
            text: badge.label
            color: Kirigami.Theme.textColor
            font.weight: Font.DemiBold
        }
    }

    QQC2.Frame {
        Layout.fillWidth: true
        ColumnLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.largeSpacing
            RowLayout {
                Layout.fillWidth: true
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    text: qsTr("Stream readiness")
                }
                QQC2.BusyIndicator {
                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                    implicitHeight: implicitWidth
                    running: panel.checking
                    visible: running
                }
            }
            Kirigami.Heading {
                Layout.fillWidth: true
                visible: panel.results.length > 0
                level: 3
                wrapMode: Text.WordWrap
                text: panel.checking ? qsTr("Checking live observations…") : panel.attentionCount === 1 ? qsTr("1 check needs attention") : panel.attentionCount > 1 ? qsTr("%1 checks need attention").arg(panel.attentionCount) : panel.unknownCount > 0 ? qsTr("Some checks could not be verified") : qsTr("Controls and routing checked")
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: panel.results.length ? qsTr("Review the groups below. Verified applies only to the stated check.") : qsTr("Inspect your current controls, devices, routing and OBS captures.")
                color: Kirigami.Theme.textColor
            }
            GridLayout {
                Layout.fillWidth: true
                visible: panel.results.length > 0
                columns: panel.width < Kirigami.Units.gridUnit * 32 ? 2 : 4
                columnSpacing: Kirigami.Units.largeSpacing * 2
                rowSpacing: Kirigami.Units.smallSpacing
                Repeater {
                    model: [
                        {
                            count: panel.attentionCount,
                            status: "attention",
                            label: qsTr("Needs attention")
                        },
                        {
                            count: panel.unknownCount,
                            status: "unknown",
                            label: qsTr("Not verified")
                        },
                        {
                            count: panel.verifiedCount,
                            status: "verified",
                            label: qsTr("Verified")
                        },
                        {
                            count: panel.excludedCount,
                            status: "excluded",
                            label: qsTr("Excluded / idle")
                        }
                    ]
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Kirigami.Icon {
                            source: panel.statusIcon(modelData.status)
                            color: panel.statusColor(modelData.status)
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: implicitWidth
                            Accessible.ignored: true
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: modelData.count + " " + modelData.label
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
            QQC2.Button {
                objectName: "checkReadiness"
                text: panel.results.length ? qsTr("Check again") : qsTr("Check stream readiness")
                icon.name: "view-refresh"
                enabled: !panel.checking
                onClicked: panel.checkRequested()
            }
        }
    }

    QQC2.Frame {
        Layout.fillWidth: true
        visible: panel.attentionRows.length > 0
        padding: 0
        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            Kirigami.Heading {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                level: 3
                text: qsTr("Needs attention")
            }
            Repeater {
                objectName: "attentionRows"
                model: panel.attentionRows
                delegate: ResultEvidence {}
            }
        }
    }

    Repeater {
        model: panel.definitions
        delegate: QQC2.Frame {
            id: section
            required property var modelData
            required property int index
            readonly property var rows: panel.groups[index].rows
            readonly property int problems: rows.filter(r => r.status === "attention").length
            readonly property int unknowns: rows.filter(r => r.status === "unknown").length
            readonly property bool hasIssues: problems > 0 || unknowns > 0
            property bool expanded: hasIssues
            onHasIssuesChanged: {
                if (hasIssues)
                    expanded = true;
            }
            objectName: "group-" + modelData.key
            Layout.fillWidth: true
            visible: rows.length > 0
            padding: 0
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                QQC2.ItemDelegate {
                    focusPolicy: Qt.StrongFocus
                    objectName: "toggle-" + section.modelData.key
                    Layout.fillWidth: true
                    onClicked: section.expanded = !section.expanded
                    Accessible.name: section.modelData.title
                    Accessible.checkable: true
                    Accessible.checked: section.expanded
                    Accessible.description: section.hasIssues ? qsTr("Contains checks needing review") : qsTr("No checks need attention")
                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: section.modelData.icon
                            implicitWidth: Kirigami.Units.iconSizes.smallMedium
                            implicitHeight: implicitWidth
                            Accessible.ignored: true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            QQC2.Label {
                                text: section.modelData.title
                                font.weight: Font.DemiBold
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                font: Kirigami.Theme.smallFont
                                color: Kirigami.Theme.textColor
                                wrapMode: Text.WordWrap
                                text: section.problems > 0 ? qsTr("Needs attention: %1 · Checks: %2").arg(section.problems).arg(section.rows.length) : section.unknowns > 0 ? qsTr("%1 not verified · %2 checks").arg(section.unknowns).arg(section.rows.length) : qsTr("%1 checks · verified or intentionally excluded/idle").arg(section.rows.length)
                            }
                        }
                        Kirigami.Icon {
                            source: section.expanded ? "arrow-down" : "arrow-right"
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: implicitWidth
                            Accessible.ignored: true
                        }
                    }
                }
                Repeater {
                    model: section.expanded ? section.rows : []
                    delegate: ResultEvidence {}
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.largeSpacing
        Kirigami.Icon {
            Layout.alignment: Qt.AlignTop
            source: "dialog-information"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
            Accessible.ignored: true
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: qsTr("Recording and audience audio are not verified. This check does not play or record sound, or change settings. Confirm the final result with a short recording.")
        }
    }
    QQC2.Button {
        text: qsTr("Limits of this check")
        icon.name: panel.scopeOpen ? "arrow-down" : "arrow-right"
        onClicked: panel.scopeOpen = !panel.scopeOpen
        Accessible.checkable: true
        Accessible.checked: panel.scopeOpen
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: panel.scopeOpen
        text: panel.results.find(r => r.id === "output-proof")?.detail ?? qsTr("Recording contents, output track selection and audience audio are not verified.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
}
