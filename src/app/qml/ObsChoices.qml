import QtQuick
import QtQuick.Layouts
import org.kde.kirigamiaddons.formcard as FormCard
import Rostrum

// "Follow OBS" and go-live warnings, shared by setup, the setup update dialog and Settings.
FormCard.FormCard {
    maximumWidth: width - 2

    FormCard.FormSwitchDelegate {
        text: i18nc("@option:check", "Follow OBS while it runs")
        description: i18n("Rostrum stays connected to OBS's WebSocket server on this computer while OBS is open. It shows LIVE and REC in the header, warns you when a stream starts with a problem, and follows OBS scene changes. It only reads from OBS, and nothing leaves your computer.")
        checked: Obs.background
        onToggled: Obs.background = checked
    }
    FormCard.FormDelegateSeparator {}
    FormCard.FormSwitchDelegate {
        text: i18nc("@option:check", "Warn me when a stream starts with a problem")
        description: i18n("A banner and a notification if your mic is muted, nothing reaches the stream, or OBS isn't recording Rostrum Mic and Rostrum Stream Mix.")
        enabled: Obs.background
        checked: Obs.goLiveWarnings
        onToggled: Obs.goLiveWarnings = checked
    }
}
