import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Historique des notes de version (releases GitHub). Utilisé avant une maj, après
// une maj (« quoi de neuf »), ou depuis le menu pour feuilleter le passé.
ColoDialog {
    id: dlg

    // "pending" = versions plus récentes (avant téléchargement)
    // "whatsNew" = versions installées pas encore lues (après maj)
    // "history"  = tout l'historique
    property string mode: "history"

    title: mode === "pending" ? ("Nouveautés — " + Updater.latestVersion)
         : mode === "whatsNew" ? ("Quoi de neuf — " + Updater.currentVersion)
         : "Notes de version"
    acceptText: mode === "pending" ? "Télécharger"
              : mode === "whatsNew" ? "Compris"
              : "Fermer"
    showCancel: mode === "pending"
    destructive: false

    readonly property string bodyText: {
        if (mode === "pending")
            return Updater.releaseNotes
        if (mode === "whatsNew")
            return Updater.whatsNewNotes
        // Historique : on assemble depuis la liste structurée.
        let blocks = []
        for (let i = 0; i < Updater.changelog.length; ++i) {
            const e = Updater.changelog[i]
            const ver = e.version || ""
            const notes = (e.notes || "").trim()
            if (!ver)
                continue
            blocks.push(notes.length > 0
                        ? ("Version " + ver + "\n\n" + notes)
                        : ("Version " + ver))
        }
        return blocks.join("\n\n————————————\n\n")
    }

    function openPending()  { mode = "pending";  open() }
    function openWhatsNew() { mode = "whatsNew"; open() }
    function openHistory()  { mode = "history";  open() }

    Label {
        Layout.fillWidth: true
        visible: dlg.bodyText.length === 0
        text: mode === "history"
              ? "Aucune note de version pour l'instant."
              : "Corrections et améliorations."
        color: Theme.textDim
        font.pixelSize: 14
        wrapMode: Text.WordWrap
    }

    Flickable {
        Layout.fillWidth: true
        Layout.preferredHeight: visible
            ? Math.min(Math.max(notes.implicitHeight, 80), 360) : 0
        visible: dlg.bodyText.length > 0
        contentHeight: notes.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator {}

        Label {
            id: notes
            width: parent.width
            text: dlg.bodyText
            color: Theme.textDim
            font.pixelSize: 14
            wrapMode: Text.WordWrap
            lineHeight: 1.3
        }
    }

    onAccepted: {
        if (mode === "pending")
            Updater.download()
        else if (mode === "whatsNew")
            Updater.acknowledgeNotes()
    }
    onRejected: {
        // Fermer sans télécharger / sans marquer comme lu.
    }
}
