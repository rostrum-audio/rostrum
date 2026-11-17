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
    // The card shows the summary line and the failing rows; every other check, the counts and
    // the limits of the check sit behind "Show checks".
    property bool detailsOpen: false
    signal navigateRequested(string page)
    signal reviewObsRequested
    spacing: Kirigami.Units.largeSpacing

    readonly property var checks: results.filter(r => r.id !== "output-proof")
    readonly property int attentionCount: checks.filter(r => r.status === "attention").length
    readonly property int unknownCount: checks.filter(r => r.status === "unknown").length
    readonly property int verifiedCount: checks.filter(r => r.status === "verified").length
    readonly property int excludedCount: checks.filter(r => r.status === "excluded").length
    readonly property string summaryStatus: attentionCount > 0 ? "attention" : unknownCount > 0 ? "unknown" : "verified"
    readonly property bool showDetails: results.length > 0 && detailsOpen
    readonly property var definitions: [
        {
            key: "controls",
            title: i18n("Controls"),
            icon: "audio-volume-high"
        },
        {
            key: "devices",
            title: i18n("Devices"),
            icon: "audio-card"
        },
        {
            key: "routing",
            title: i18n("Routing"),
            icon: "network-connect"
        },
        {
            key: "obs",
            title: i18n("OBS capture"),
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
            return i18n("Review the microphone's effective mute, gain and stream destination.");
        if (id === "stream-level")
            return i18n("The Stream Mix is effectively silent. Review mute, gain and solo controls.");
        if (id.endsWith("-device"))
            return i18n("The selected device is missing or could not be resolved. Review the selection and fallback settings.");
        if (id.endsWith("-observed-level"))
            return i18n("Observed audio properties indicate silence where audio is expected.");
        if (id.startsWith("obs"))
            return i18n("Review the OBS capture target, mute and track assignments. Expand this result for the observed issue.");
        return i18n("Observed routing does not match the intended path. Expand this result for the missing or unexpected link.");
    }
    function correction(id) {
        if (id.startsWith("obs"))
            return {
                page: "obs",
                label: i18n("Review OBS setup")
            };
        if (id.startsWith("app-"))
            return {
                page: "apps",
                label: i18n("Review app routing")
            };
        if (id.endsWith("-device") || id === "mic-route" || id === "phones-output")
            return {
                page: "devices",
                label: i18n("Open devices")
            };
        if (id === "mic-filters")
            return {
                page: "filters",
                label: i18n("Open Studio Voice")
            };
        return {
            page: "mixer",
            label: i18n("Open mixer")
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

    // A failing check: the problem in words, and a link to the page that fixes it.
    component FailingRow: ColumnLayout {
        required property var modelData
        Layout.fillWidth: true
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.ItemDelegate {
                id: failingRow
                focusPolicy: Qt.StrongFocus
                objectName: "row-" + modelData.id
                readonly property bool detailsOpen: panel.openEvidence[modelData.id] === true
                Layout.fillWidth: true
                onClicked: panel.toggleEvidence(modelData.id, false)
                Accessible.name: modelData.title + ", " + modelData.label
                Accessible.description: detailsOpen ? i18n("Collapse observed evidence") : i18n("Expand observed evidence")
                Accessible.checkable: true
                Accessible.checked: detailsOpen
                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Icon {
                        Layout.alignment: Qt.AlignTop
                        source: panel.statusIcon(modelData.status)
                        color: panel.statusColor(modelData.status)
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: implicitWidth
                        Accessible.ignored: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: modelData.title
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: modelData.detail.length <= 180 ? modelData.detail : panel.explanation(modelData.id)
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            font: Kirigami.Theme.smallFont
                            opacity: 0.8
                        }
                    }
                    Kirigami.Icon {
                        source: failingRow.detailsOpen ? "arrow-down" : "arrow-right"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: implicitWidth
                        Accessible.ignored: true
                    }
                }
            }
            QQC2.Button {
                objectName: "correct-" + modelData.id
                flat: true
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
        }
        QQC2.Label {
            objectName: "detail-" + modelData.id
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.topMargin: 0
            visible: failingRow.detailsOpen
            text: modelData.detail
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: Kirigami.Theme.textColor
        }
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
            Accessible.description: detailsOpen ? i18n("Collapse observed evidence") : i18n("Expand observed evidence")
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

    // One-line summary: icon and words, never colour alone.
    RowLayout {
        Layout.fillWidth: true
        visible: panel.results.length > 0
        spacing: Kirigami.Units.smallSpacing
        Kirigami.Icon {
            source: panel.statusIcon(panel.summaryStatus)
            color: panel.statusColor(panel.summaryStatus)
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
            visible: !panel.checking
            Accessible.ignored: true
        }
        QQC2.BusyIndicator {
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: implicitWidth
            running: panel.checking
            visible: running
        }
        QQC2.Label {
            objectName: "readinessSummary"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.weight: Font.DemiBold
            text: panel.checking ? i18n("Checking live observations…") : panel.attentionCount > 0 ? i18np("1 check needs attention", "%1 checks need attention", panel.attentionCount) : panel.unknownCount > 0 ? i18n("Some checks could not be verified") : i18n("Controls and routing checked")
        }
        QQC2.ToolButton {
            objectName: "showChecks"
            text: panel.detailsOpen ? i18n("Hide checks") : i18n("Show checks")
            icon.name: panel.detailsOpen ? "arrow-up" : "arrow-down"
            onClicked: panel.detailsOpen = !panel.detailsOpen
            Accessible.name: i18n("Show checks")
            Accessible.description: panel.detailsOpen ? i18n("Expanded") : i18n("Collapsed")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.attentionRows.length > 0
        spacing: 0
        Repeater {
            objectName: "attentionRows"
            model: panel.attentionRows
            delegate: FailingRow {}
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        visible: panel.showDetails
        wrapMode: Text.WordWrap
        text: i18n("Review the groups below. Verified applies only to the stated check.")
        color: Kirigami.Theme.textColor
    }
    GridLayout {
        Layout.fillWidth: true
        visible: panel.showDetails
        columns: panel.width < Kirigami.Units.gridUnit * 32 ? 2 : 4
        columnSpacing: Kirigami.Units.largeSpacing * 2
        rowSpacing: Kirigami.Units.smallSpacing
        Repeater {
            model: [
                {
                    count: panel.attentionCount,
                    status: "attention",
                    label: i18n("Needs attention")
                },
                {
                    count: panel.unknownCount,
                    status: "unknown",
                    label: i18n("Not verified")
                },
                {
                    count: panel.verifiedCount,
                    status: "verified",
                    label: i18n("Verified")
                },
                {
                    count: panel.excludedCount,
                    status: "excluded",
                    label: i18n("Excluded / idle")
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

    Repeater {
        model: panel.definitions
        delegate: ColumnLayout {
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
            visible: panel.showDetails && rows.length > 0
            spacing: 0
            Kirigami.Separator {
                Layout.fillWidth: true
            }
            QQC2.ItemDelegate {
                focusPolicy: Qt.StrongFocus
                objectName: "toggle-" + section.modelData.key
                Layout.fillWidth: true
                onClicked: section.expanded = !section.expanded
                Accessible.name: section.modelData.title
                Accessible.checkable: true
                Accessible.checked: section.expanded
                Accessible.description: section.hasIssues ? i18n("Contains checks needing review") : i18n("No checks need attention")
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
                        spacing: 0
                        QQC2.Label {
                            text: section.modelData.title
                            font.weight: Font.DemiBold
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.textColor
                            wrapMode: Text.WordWrap
                            text: section.problems > 0 ? i18n("Needs attention: %1 · Checks: %2", section.problems, section.rows.length) : section.unknowns > 0 ? i18n("%1 not verified · %2 checks", section.unknowns, section.rows.length) : i18n("%1 checks · verified or intentionally excluded/idle", section.rows.length)
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

    Kirigami.Separator {
        Layout.fillWidth: true
        visible: panel.showDetails
    }
    RowLayout {
        Layout.fillWidth: true
        visible: panel.showDetails
        spacing: Kirigami.Units.smallSpacing
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
            text: i18n("Recording and audio for viewers are not verified. This check does not play or record sound, or change settings. Confirm the final result with a short recording.")
        }
    }
    QQC2.ToolButton {
        visible: panel.showDetails
        text: i18n("Limits of this check")
        icon.name: panel.scopeOpen ? "arrow-down" : "arrow-right"
        onClicked: panel.scopeOpen = !panel.scopeOpen
        Accessible.checkable: true
        Accessible.checked: panel.scopeOpen
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: panel.showDetails && panel.scopeOpen
        text: panel.results.find(r => r.id === "output-proof")?.detail ?? i18n("Recording contents, output track selection and audio for viewers are not verified.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
}
