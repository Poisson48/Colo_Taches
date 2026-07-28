import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Choix de la catégorie. La liste vient du modèle (C++) : les catégories d’origine,
// où on traverse un magasin, puis ceux que les participants ont inventés dans cette
// liste. « Sans catégorie » est une valeur réelle (chaîne vide), pas l'absence de choix.
ComboBox {
    id: box

    // La catégorie choisie, "" pour « Sans catégorie ».
    property string aisle: ""

    // Émis quand l'utilisateur choisit LUI-MÊME une catégorie (pas un pré-remplissage) :
    // la barre d'ajout s'en sert pour cesser d'écraser son choix.
    signal chosen()

    readonly property string newLabel: "Nouvelle catégorie…"

    readonly property var options: {
        let list = ["Sans catégorie"].concat(AppController.items.aisleNames)
        // Catégorie fraîchement tapée : la tâche n’est pas encore ajoutée, donc le modèle ne
        // le connaît pas — sans ça, le champ retomberait sur « Sans catégorie » sous les
        // yeux de l'utilisateur, juste après qu'il l'a saisi.
        if (aisle.length > 0 && list.indexOf(aisle) < 0)
            list.push(aisle)
        return list.concat([newLabel])
    }

    implicitHeight: 44
    model: options

    // Assigner `aisle` de l'extérieur (dialogue d'édition) doit bouger la sélection.
    onAisleChanged: {
        const wanted = aisle.length > 0 ? aisle : "Sans catégorie"
        const at = options.indexOf(wanted)
        if (at >= 0 && at !== currentIndex)
            currentIndex = at
    }

    onActivated: function (index) {
        if (options[index] === newLabel) {
            newAisleDialog.open()
            return
        }
        aisle = (index === 0) ? "" : options[index]
        box.chosen()
    }

    ColoDialog {
        id: newAisleDialog
        title: "Nouvelle catégorie"
        acceptText: "Créer"
        acceptEnabled: newAisleField.text.trim().length > 0

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 13
            text: "La catégorie sera proposée pour les autres tâches de cette liste, "
                  + "et les participants le verront aussi."
        }

        ColoTextField {
            id: newAisleField
            Layout.fillWidth: true
            hint: "Garage, Piscine, Voiture…"
            onAccepted: if (newAisleDialog.acceptEnabled) newAisleDialog.accept()
        }

        onOpened: { newAisleField.text = ""; newAisleField.forceActiveFocus() }

        onAccepted: { box.aisle = newAisleField.text.trim(); box.chosen() }
        // Refusé : le champ affichait déjà « Nouvelle catégorie… », il faut le remettre sur
        // la catégorie réellement choisie.
        onRejected: box.currentIndex = Math.max(0, box.options.indexOf(
                        box.aisle.length > 0 ? box.aisle : "Sans catégorie"))
    }

    background: Rectangle {
        radius: 10
        color: Theme.surfaceHigh
        border.color: box.activeFocus ? Theme.accent : Theme.outline
        border.width: 1
    }

    contentItem: Label {
        leftPadding: 10
        rightPadding: box.indicator.width
        text: box.displayText
        color: box.aisle.length > 0 ? Theme.text : Theme.textDim
        font.pixelSize: 13
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    popup: Popup {
        y: box.height + 4
        width: Math.max(box.width, 190)
        implicitHeight: Math.min(contentItem.implicitHeight + 16, 320)
        padding: 8

        background: Rectangle {
            radius: 12
            color: Theme.surface
            border.color: Theme.outline
            border.width: 1
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: box.popup.visible ? box.delegateModel : null
            currentIndex: box.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }

    // Pas de `required property` ici : déclarer une propriété requise ferait basculer
    // le délégué en mode « propriétés requises », et Qt cesserait d'injecter le contexte
    // du modèle — `modelData` deviendrait introuvable et la liste des catégories s’afficherait
    // vide. Le contexte suffit largement pour un modèle qui n'est qu'un tableau.
    delegate: ItemDelegate {
        id: option
        width: box.popup.width - 16
        height: 42

        readonly property bool isNew: modelData === box.newLabel

        contentItem: Label {
            text: modelData
            // « Nouvelle catégorie… » est une action, pas une catégorie : elle se lit comme telle.
            color: option.isNew ? Theme.accent : Theme.text
            font.pixelSize: 14
            font.weight: option.isNew ? Font.DemiBold : Font.Normal
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: 8
            color: option.hovered || box.currentIndex === index
                   ? Theme.surfaceHigh : "transparent"
        }

        // Le délégué est personnalisé : c'est lui qui traite le clic, onActivated n'est
        // pas émis. La création d'une catégorie doit donc être gérée ici aussi.
        onClicked: {
            box.popup.close()
            if (option.isNew) {
                newAisleDialog.open()
                return
            }
            box.aisle = (index === 0) ? "" : box.options[index]
            box.currentIndex = index
            box.chosen()
        }
    }
}
