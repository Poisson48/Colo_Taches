import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: root

    required property string listId
    required property string listTitle

    // Sélection multiple : vide = mode normal. Un appui long sur une ligne y entre.
    property var selectedIds: []
    readonly property bool selectionMode: selectedIds.length > 0

    // Mode Chantier : on est en plein travail, les mains prises. Plus rien à faire
    // que cocher ce qui est terminé — pas d'ajout, pas de suppression, pas de page
    // de détail ouverte par mégarde, et toute la ligne devient la case à cocher.
    property bool shoppingMode: false

    // Barre de recherche ouverte. Le filtre vit dans le modèle (C++) : il ne masque
    // que l'affichage, il ne supprime ni ne désynchronise rien.
    property bool searchOpen: false

    // Vrai le temps qu'une poignée de déplacement est tenue : fige le défilement de la
    // liste pour que le glissement l'emporte sur écran tactile.
    property bool rowDragging: false

    readonly property string pageTitle: selectionMode
        ? selectedIds.length + (selectedIds.length > 1 ? " sélectionnés" : " sélectionné")
        : listTitle

    // En plein boulot, on pose le téléphone sans le toucher pendant des minutes :
    // l'écran ne doit pas s'éteindre (déjà géré globalement dans Main) et les étapes
    // se déplient sous leurs tâches, cochables en place.
    onShoppingModeChanged: {
        AppController.items.showSteps = shoppingMode
    }
    // Quitter la page en mode Au boulot laisserait les étapes dépliées au prochain écran.
    Component.onDestruction: {
        AppController.items.showSteps = false
    }

    function closeSearch() {
        searchOpen = false
        searchField.text = ""
        AppController.items.filter = ""
    }

    // Retour (bouton Android ou flèche) : défaire l'état courant avant de quitter la
    // page — la sélection, puis la recherche, puis le mode Chantier.
    function handleBack() {
        if (selectionMode) {
            selectedIds = []
            return true
        }
        if (searchOpen) {
            closeSearch()
            return true
        }
        if (shoppingMode) {
            shoppingMode = false
            return true
        }
        return false
    }

    function isSelected(itemId) {
        return selectedIds.indexOf(itemId) >= 0
    }

    function toggleSelection(itemId) {
        // Réassignation d'un nouveau tableau : muter en place ne notifierait personne.
        const next = selectedIds.slice()
        const at = next.indexOf(itemId)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(itemId)
        selectedIds = next
    }

    // Le titre suit le renommage, d'ici ou d'un autre appareil.
    Connections {
        target: AppController
        function onListRenamed(listId, title) {
            if (listId === root.listId)
                root.listTitle = title
        }
    }

    property Component actions: Row {
        spacing: 0

        ToolButton {
            width: 96
            height: Theme.touchTarget
            visible: root.selectionMode
            contentItem: Label {
                text: "Supprimer"
                color: Theme.danger
                font.pixelSize: 14
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: deleteDialog.openFor(root.selectedIds)
        }

        // En mode Chantier, une seule sortie possible, bien visible.
        ToolButton {
            width: 92
            height: Theme.touchTarget
            visible: root.shoppingMode && !root.selectionMode
            contentItem: Label {
                text: "Terminer"
                color: Theme.accent
                font.pixelSize: 14
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: root.shoppingMode = false
        }

        // Le mode des mains occupées est celui qu'on ouvre le plus souvent : il
        // mérite un bouton, pas une ligne de menu.
        ToolButton {
            width: 110
            height: Theme.touchTarget
            visible: !root.selectionMode && !root.shoppingMode
            contentItem: Label {
                text: "Au boulot !"
                color: Theme.accent
                font.pixelSize: 14
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: root.shoppingMode = true
        }

        ToolButton {
            width: Theme.touchTarget
            height: Theme.touchTarget
            visible: !root.selectionMode && !root.shoppingMode
            contentItem: Icon {
                name: "menu"
                color: Theme.text
                size: 18
            }
            onClicked: pageMenu.popup()
        }
    }

    // Gestion d'une suggestion de favori (appui long sur une pastille).
    ColoMenu {
        id: favMenu
        property string favName: ""
        property bool   pinned: false

        MenuItem {
            text: favMenu.pinned ? "Ne plus épingler" : "Épingler en tête"
            onTriggered: AppController.pinFavorite(favMenu.favName, !favMenu.pinned)
        }
        MenuItem {
            text: "Retirer des suggestions"
            onTriggered: AppController.removeFavorite(favMenu.favName)
        }
    }

    ColoMenu {
        id: pageMenu
        objectName: "pageMenu"

        MenuItem {
            text: "Rechercher"
            onTriggered: root.searchOpen = true
        }
        // Ce qui a déjà été fait, et par qui — y compris les tâches retirées depuis.
        MenuItem {
            text: "Historique"
            onTriggered: root.StackView.view.push(historyPageComponent,
                                                  { listId: root.listId })
        }
        // Sans ces deux-là, les tâches cochées restent barrées à l'écran pour
        // toujours, et il faut les traiter une par une.
        MenuItem {
            text: "Tout remettre à faire"
            enabled: AppController.items.doneCount > 0
            onTriggered: uncheckDialog.open()
        }
        MenuItem {
            text: "Retirer les tâches faites"
            enabled: AppController.items.doneCount > 0
            onTriggered: clearDoneDialog.open()
        }
        MenuSeparator {}
        // Classement (partagé avec les autres participants) : un seul interrupteur,
        // pour qu'il y ait toujours un mode actif (coché = manuel, décoché = par
        // catégorie). Deux cases séparées permettaient de tout décocher.
        MenuItem {
            text: "Classement manuel"
            checkable: true
            checked: AppController.items.manualSort
            onTriggered: AppController.items.manualSort = checked
        }
        MenuSeparator {}
        MenuItem {
            text: "Partager la liste"
            onTriggered: shareSheet.openFor(root.listId, root.listTitle)
        }
        MenuItem {
            text: "Renommer la liste"
            onTriggered: renameDialog.open()
        }
        MenuItem {
            text: "Dupliquer la liste"
            onTriggered: duplicateDialog.open()
        }
    }

    ShareSheet { id: shareSheet }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Mode Chantier : où en est le travail, sans avoir à compter les lignes.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.shoppingMode ? 52 : 0
            visible: Layout.preferredHeight > 0
            clip: true
            color: Theme.surface
            Behavior on Layout.preferredHeight { NumberAnimation { duration: 140 } }

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.pad
                anchors.rightMargin: Theme.pad
                anchors.topMargin: 8
                anchors.bottomMargin: 10
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    text: AppController.items.count === 0
                          ? "Liste vide"
                          : (AppController.items.doneCount === AppController.items.count
                             ? "Tout est fait !"
                             : AppController.items.doneCount + " sur "
                               + AppController.items.count + " faites")
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    radius: 2
                    color: Theme.surfaceHigh

                    Rectangle {
                        height: parent.height
                        radius: 2
                        color: Theme.accent
                        width: AppController.items.count > 0
                               ? parent.width * (AppController.items.doneCount
                                                 / AppController.items.count)
                               : 0
                        Behavior on width { NumberAnimation { duration: 180 } }
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.outline
            }
        }

        // Recherche : au-delà d'une vingtaine de tâches, retrouver « ampoule » à
        // l'œil devient pénible.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.searchOpen ? 60 : 0
            visible: Layout.preferredHeight > 0
            clip: true
            color: Theme.surface
            Behavior on Layout.preferredHeight { NumberAnimation { duration: 140 } }

            onVisibleChanged: {
                if (visible)
                    searchField.forceActiveFocus()
            }

            RowLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                ColoTextField {
                    id: searchField
                    Layout.fillWidth: true
                    hint: "Rechercher une tâche"
                    onTextChanged: AppController.items.filter = text
                }

                ToolButton {
                    Layout.preferredWidth: Theme.touchTarget
                    Layout.preferredHeight: Theme.touchTarget
                    contentItem: Icon {
                        name: "close"
                        color: Theme.textDim
                        size: 17
                    }
                    onClicked: root.closeSearch()
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.outline
            }
        }

        ListView {
            id: items
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: AppController.items
            // La vue ne se fige QUE pendant qu'une poignée est tenue : sinon elle
            // disputerait le geste vertical au glissement. Au repos, elle défile.
            interactive: !root.rowDragging
            topMargin: Theme.gap
            bottomMargin: Theme.gap
            spacing: 6

            // Sections par catégorie. Elles n'apparaissent que si les tâches sont
            // effectivement classées : qui ne s'en sert pas ne voit aucun en-tête. En
            // mode manuel, aucune section : la liste est une seule séquence à la main.
            section.property: AppController.items.manualSort ? "" : "aisle"
            section.criteria: ViewSection.FullString

            // Un changement reçu d'un autre téléphone recharge le modèle en bloc
            // (reset), ce qui renvoie la vue tout en haut. À deux sur le chantier,
            // chaque cochage de l'autre ferait perdre sa place. On mémorise la
            // position juste avant le reset et on la rétablit juste après (bornée
            // si la liste a raccourci).
            property real savedContentY: 0
            Connections {
                target: AppController.items
                function onModelAboutToBeReset() { items.savedContentY = items.contentY }
                function onModelReset() {
                    Qt.callLater(function() {
                        items.contentY = items.savedContentY
                        items.returnToBounds()
                    })
                }
            }
            section.delegate: Item {
                required property string section
                width: items.width
                height: AppController.items.aisleCount > 1 ? 34 : 0
                visible: height > 0

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.gap + 4
                    anchors.verticalCenter: parent.verticalCenter
                    // La catégorie vide est réelle (« non classé »), il lui faut un nom.
                    text: parent.section.length > 0 ? parent.section : "Sans catégorie"
                    color: Theme.accent
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                }
            }

            // Un article coché ne bouge pas (le tri du modèle ignore l'état fait) :
            // il s'estompe sur place — la liste ne se réorganise pas sous les doigts.
            //
            // Conteneur immobile (place dans la vue + zone de dépôt) contenant la ligne,
            // qui se détache sous le doigt pendant un glissement.
            delegate: Item {
                id: wrapper
                width: items.width
                // Hauteur = contenu (nom qui peut passer à la ligne).
                height: row.height

                // Pas de `required property int index` (couperait le contexte du modèle).
                property int rowIndex: index
                // « Saisie » : posée par la poignée dès l'appui. Fige la vue le temps du
                // glissement (root.rowDragging) pour que le geste l'emporte sur le
                // défilement, et détache la ligne sous le doigt.
                property bool held: false
                z: held ? 2 : 1

                DropArea {
                    anchors.fill: parent
                    onEntered: function (drag) {
                        const source = drag.source
                        if (!source || source === wrapper)
                            return
                        // Franchir une frontière de catégorie y range la tâche :
                        // c'est le modèle qui en décide (moveItem), pas la vue.
                        AppController.items.moveItem(source.rowIndex, wrapper.rowIndex)
                    }
                }

                SwipeDelegate {
                    id: row
                    readonly property int stepIndent: model.isStep ? 26 : 0
                    width: wrapper.width - 2 * Theme.gap - stepIndent
                    x: Theme.gap + stepIndent
                    // S'adapte au nom enroulé (WordWrap) : une ligne courte reste compacte.
                    height: Math.max(root.shoppingMode ? (model.isStep ? 52 : 68) : 60,
                                     contentItem.implicitHeight + 8)
                    padding: 0

                    readonly property bool selected: root.isSelected(model.itemId)

                    Drag.active: wrapper.held
                    Drag.source: wrapper
                    Drag.hotSpot.x: width / 2
                    Drag.hotSpot.y: height / 2
                    opacity: wrapper.held ? 0.85 : 1.0
                    // Pendant le glissement, la ligne quitte son conteneur pour la vue :
                    // elle reste sous le doigt pendant que les autres se réorganisent.
                    states: State {
                        when: wrapper.held
                        ParentChange { target: row; parent: items }
                    }

                    // Le swipe supprime : désactivé en sélection (conflit) et en mode
                    // Chantier (un geste de travers effacerait la tâche des autres).
                    swipe.enabled: !root.selectionMode && !root.shoppingMode

                    background: Rectangle {
                        radius: 12
                        color: row.selected ? Theme.accentSoft
                             : (row.pressed ? Theme.surfaceHigh : Theme.surface)
                        border.color: row.selected ? Theme.accent : Theme.outline
                        border.width: 1
                    }

                    contentItem: RowLayout {
                        spacing: 0

                        CheckBox {
                            Layout.leftMargin: 6
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: Theme.touchTarget
                            implicitHeight: Theme.touchTarget
                            // En mode sélection, la case coche la ligne ; sinon elle
                            // marque la tâche comme faite.
                            checked: root.selectionMode ? row.selected : model.done
                            Material.accent: Theme.accent
                            // onToggled et pas onCheckedChanged : ce dernier repart en
                            // boucle quand le modèle se recharge après un merge distant.
                            onToggled: {
                                if (root.selectionMode)
                                    root.toggleSelection(model.itemId)
                                else
                                    AppController.items.toggleDone(model.itemId)
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            Layout.leftMargin: 4
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                text: model.name
                                color: model.done ? Theme.textDim : Theme.text
                                font.pixelSize: model.isStep ? 15 : 16
                                font.strikeout: model.done
                                wrapMode: Text.WordWrap
                            }

                            // Échéance, progression des étapes et détails.
                            Label {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: {
                                    const parts = []
                                    if (model.dueNote && model.dueNote.length > 0)
                                        parts.push(model.dueNote)
                                    else if (model.due > 0)
                                        parts.push(root.dueText(model.due))
                                    if (model.stepsTotal > 0)
                                        parts.push(model.stepsDone + "/" + model.stepsTotal
                                                   + " étape" + (model.stepsTotal > 1 ? "s" : ""))
                                    if (model.note && model.note.length > 0)
                                        parts.push(model.note)
                                    return parts.join(" \u00b7 ")
                                }
                                // Le retard crie (dates seules), le reste chuchote.
                                color: root.isOverdue(model.due) && !model.done
                                       && !(model.dueNote && model.dueNote.length > 0)
                                       ? Theme.danger
                                       : (model.stepsTotal > 0
                                          && model.stepsDone === model.stepsTotal
                                          ? Theme.accent : Theme.textDim)
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                        }

                        // Vignette de la première photo, si la tâche en a et que le
                        // blob est déjà arrivé (sinon rien, jusqu'à la prochaine révision).
                        Image {
                            Layout.preferredWidth: visible ? 40 : 0
                            Layout.preferredHeight: 40
                            Layout.rightMargin: visible ? 4 : 0
                            Layout.alignment: Qt.AlignVCenter
                            visible: model.image.length > 0 && status === Image.Ready
                            source: model.image.length > 0
                                    ? "image://taskimg/" + model.image.split(" ")[0]
                                      + "?r=" + AppController.imageRevision
                                    : ""
                            sourceSize.width: 80
                            sourceSize.height: 80
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }

                        // Poignée de déplacement, toujours visible (hors sélection et
                        // Chantier) : on la glisse pour classer la tâche, loin d'un coup,
                        // sans passer par un menu. `held` fige la vue le temps du geste.
                        MouseArea {
                            id: dragHandle
                            Layout.rightMargin: 2
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth: visible ? 34 : 0
                            Layout.preferredHeight: Theme.touchTarget
                            visible: !root.selectionMode && !root.shoppingMode
                            drag.target: wrapper.held ? row : undefined
                            drag.axis: Drag.YAxis
                            cursorShape: Qt.SizeVerCursor
                            preventStealing: true
                            onPressed: { wrapper.held = true; root.rowDragging = true }
                            onReleased: { wrapper.held = false; root.rowDragging = false }
                            onCanceled: { wrapper.held = false; root.rowDragging = false }

                            Icon {
                                anchors.centerIn: parent
                                name: "grip"
                                color: Theme.textDim
                                size: 16
                            }
                        }
                    }

                    // Appui long sur la ligne : sélection multiple (sauf en Chantier).
                    onPressAndHold: {
                        if (!root.shoppingMode)
                            root.toggleSelection(model.itemId)
                    }
                    // Mode Chantier : toute la ligne coche. Sinon : ouvrir la page de
                    // la tâche, ou étendre la sélection si elle est commencée.
                    onClicked: {
                        if (root.selectionMode) {
                            root.toggleSelection(model.itemId)
                        } else if (root.shoppingMode) {
                            AppController.items.toggleDone(model.itemId)
                            // On coche sans lever les yeux du travail : la vibration
                            // confirme le geste à la place du regard.
                            AppController.vibrate()
                        } else {
                            root.openTask(model.itemId)
                        }
                    }

                    // Glisser vers la gauche : suppression, avec un fond rouge explicite.
                    swipe.right: Rectangle {
                        width: parent.width
                        height: parent.height
                        radius: 12
                        color: Theme.danger

                        Label {
                            anchors.right: parent.right
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Supprimer"
                            color: "white"
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                        }

                        SwipeDelegate.onClicked: {
                            // La ligne est rouverte : si la confirmation est refusée,
                            // la tâche ne doit pas rester en position « glissée ».
                            row.swipe.close()
                            deleteDialog.openFor([model.itemId], model.name)
                        }
                    }
                }   // SwipeDelegate
            }   // wrapper
        }   // ListView

        // État vide. Une liste vide et une recherche sans résultat ne se disent pas
        // pareil : « ajoutez une tâche » quand on cherche « ampoule » serait absurde.
        ColumnLayout {
            visible: items.count === 0
            Layout.alignment: Qt.AlignCenter
            Layout.fillHeight: true
            Layout.leftMargin: 40
            Layout.rightMargin: 40
            spacing: 6

            readonly property bool filtering: AppController.items.filter.length > 0

            Item { Layout.fillHeight: true }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: parent.filtering ? "Aucun résultat" : "Liste vide"
                color: Theme.text
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: parent.filtering
                      ? "Aucune tâche ne correspond à « " + AppController.items.filter + " »."
                      : "Ajoutez une première tâche ci-dessous."
                color: Theme.textDim
                font.pixelSize: 14
            }

            Item { Layout.fillHeight: true }
        }

        // Favoris : tâches récurrentes (sortir les poubelles, arroser…), à remettre
        // d'un tap. Appris à l'usage (le C++ les classe par fréquence), masqués tant
        // qu'il n'y en a pas — invisibles au début, puis de plus en plus utiles.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: favBar.visibleBar ? 46 : 0
            visible: implicitHeight > 0
            clip: true
            color: Theme.surface
            Behavior on implicitHeight { NumberAnimation { duration: 120 } }

            property alias visibleBar: favBar.visibleBar

            Rectangle { width: parent.width; height: 1; color: Theme.outline }

            ListView {
                id: favBar
                anchors.fill: parent
                anchors.leftMargin: Theme.gap
                anchors.rightMargin: Theme.gap
                orientation: ListView.Horizontal
                spacing: 8
                clip: true
                model: AppController.favorites

                readonly property bool visibleBar: count > 0 && !root.selectionMode
                                                   && !root.shoppingMode && !root.searchOpen

                delegate: Rectangle {
                    required property var modelData
                    height: 34
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: favLabel.implicitWidth + 34
                    radius: 17
                    color: chipMouse.pressed ? Theme.accentSoft : Theme.surfaceHigh
                    border.color: Theme.outline
                    border.width: 1

                    Row {
                        anchors.centerIn: parent
                        spacing: 4

                        Label {
                            text: "＋"
                            color: Theme.accent
                            font.pixelSize: 14
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            id: favLabel
                            text: modelData.name
                            color: Theme.text
                            font.pixelSize: 14
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    MouseArea {
                        id: chipMouse
                        anchors.fill: parent
                        onClicked: root.addFavorite(modelData)
                        // Appui long : gérer la suggestion (épingler / retirer).
                        onPressAndHold: {
                            favMenu.favName = modelData.name
                            favMenu.pinned = modelData.pinned === true
                            favMenu.popup()
                        }
                    }
                }
            }
        }

        // Bouton d'ajout : ouvre une modale centrée (évite que le clavier cache
        // la saisie en bas d'une longue liste).
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 72
            visible: !root.selectionMode && !root.shoppingMode
            color: Theme.surface

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.outline
            }

            Button {
                anchors.fill: parent
                anchors.margins: 10
                flat: true
                contentItem: Label {
                    text: "＋  Ajouter une tâche"
                    color: Theme.accent
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 12
                    color: parent.pressed ? Theme.surfaceHigh : Theme.background
                    border.color: Theme.outline
                    border.width: 1
                }
                onClicked: addItemDialog.open()
            }
        }
    }

    // Barre de sélection : rappelle ce qui est sélectionné et comment en sortir.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 60
        visible: root.selectionMode
        color: Theme.surfaceHigh

        Rectangle { width: parent.width; height: 1; color: Theme.outline }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.pad
            anchors.rightMargin: 10
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: root.selectedIds.length + (root.selectedIds.length > 1
                                                 ? " tâches sélectionnées"
                                                 : " tâche sélectionnée")
                color: Theme.text
                font.pixelSize: 14
                elide: Text.ElideRight
            }

            Button {
                flat: true
                implicitHeight: Theme.touchTarget
                contentItem: Label {
                    text: "Annuler"
                    color: Theme.textDim
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: root.selectedIds = []
            }

            Button {
                flat: true
                implicitHeight: Theme.touchTarget
                contentItem: Label {
                    text: "Supprimer"
                    color: Theme.danger
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: deleteDialog.openFor(root.selectedIds)
            }
        }
    }

    // L'utilisateur a-t-il choisi la catégorie lui-même pour la tâche en cours ?
    property bool aisleManual: false

    ColoDialog {
        id: addItemDialog
        title: "Nouvelle tâche"
        acceptText: "Ajouter"
        acceptEnabled: nameField.text.trim().length > 0

        ColoTextField {
            id: nameField
            Layout.fillWidth: true
            hint: "Tâche"
            onAccepted: if (addItemDialog.acceptEnabled) addItemDialog.accept()
            onTextChanged: {
                if (root.aisleManual)
                    return
                addAisleBox.aisle = AppController.suggestAisle(text.trim())
            }
        }

        ColoTextField {
            id: addNoteField
            Layout.fillWidth: true
            hint: "Détails (facultatif)"
            onAccepted: if (addItemDialog.acceptEnabled) addItemDialog.accept()
        }

        AisleBox {
            id: addAisleBox
            objectName: "addAisleBox"
            Layout.fillWidth: true
            onChosen: root.aisleManual = true
        }

        onOpened: {
            nameField.text = ""
            addNoteField.text = ""
            root.aisleManual = false
            addAisleBox.aisle = ""
            nameField.forceActiveFocus()
        }
        onAccepted: root.addItem()
    }

    // Échéance en clair : « pour demain », « en retard (3 juil.) »… "" si aucune.
    function dueText(ms) {
        if (!ms || ms <= 0)
            return ""
        const today = new Date(); today.setHours(0, 0, 0, 0)
        const d = new Date(ms);   d.setHours(0, 0, 0, 0)
        const diff = Math.round((d - today) / 86400000)
        if (diff < 0)  return "en retard (" + Qt.formatDate(d, "d MMM") + ")"
        if (diff === 0) return "pour aujourd'hui"
        if (diff === 1) return "pour demain"
        if (diff < 7)  return "pour " + Qt.formatDate(d, "dddd")
        return "pour le " + Qt.formatDate(d, "d MMM")
    }

    function isOverdue(ms) {
        if (!ms || ms <= 0)
            return false
        const today = new Date(); today.setHours(0, 0, 0, 0)
        return ms < today.getTime()
    }

    // Ouvre la page de détail (étapes, photo, description) d'une tâche.
    function openTask(itemId) {
        AppController.openTask(itemId)
        root.StackView.view.push(taskPageComponent,
                                 { listId: root.listId, itemId: itemId })
    }

    Component {
        id: taskPageComponent
        TaskPage {}
    }

    Component {
        id: historyPageComponent
        HistoryPage {}
    }

    function addItem() {
        const name = nameField.text.trim()
        if (name.length === 0)
            return

        // Deux « Poubelles » dans une liste partagée, c'est le doublon classique :
        // chacun l'ajoute de son côté. On prévient, on n'interdit pas.
        const existing = AppController.items.existingName(name)
        if (existing.length > 0) {
            duplicateItemDialog.existingName = existing
            duplicateItemDialog.open()
            return
        }

        commitItem()
    }

    function commitItem() {
        AppController.items.addItem(nameField.text.trim(),
                                    "",
                                    addNoteField.text.trim(),
                                    addAisleBox.aisle)
        nameField.text = ""
        addNoteField.text = ""
        root.aisleManual = false
        addAisleBox.aisle = ""
        if (addItemDialog.opened)
            addItemDialog.close()
    }

    // Ajout depuis un favori : la catégorie mémorisée fait gagner la saisie. Déjà
    // présent dans la liste ? On le dit sans rien ajouter (pas de doublon silencieux).
    function addFavorite(fav) {
        const existing = AppController.items.existingName(fav.name)
        if (existing.length > 0) {
            AppController.toast("« " + existing + " » est déjà dans la liste")
            return
        }
        AppController.items.addItem(fav.name, "", "", fav.aisle)
    }

    // --- Dialogues ---

    ColoDialog {
        id: duplicateItemDialog
        title: "Déjà dans la liste"
        acceptText: "Ajouter quand même"

        property string existingName: ""

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: "« " + duplicateItemDialog.existingName + " » est déjà sur la liste. "
                  + "L'ajouter une seconde fois ?"
        }

        onAccepted: root.commitItem()
    }

    ColoDialog {
        id: uncheckDialog
        title: "Tout remettre à faire ?"
        acceptText: "Tout décocher"

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: "Les " + AppController.items.doneCount
                  + " tâches faites (et leurs étapes) repassent à faire, pour tout le "
                  + "monde. Pratique pour les corvées qui reviennent chaque semaine."
        }

        onAccepted: AppController.items.uncheckAll()
    }

    ColoDialog {
        id: clearDoneDialog
        title: "Retirer les tâches faites ?"
        acceptText: "Retirer"
        destructive: true

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: AppController.items.doneCount
                  + " tâche(s) faite(s) seront retirées de la liste — avec leurs "
                  + "étapes — pour tout le monde. Celles qui restent à faire ne bougent pas."
        }

        onAccepted: AppController.items.removeDone()
    }

    ColoDialog {
        id: deleteDialog
        title: "Supprimer ?"
        acceptText: "Supprimer"
        destructive: true

        property var    ids: []
        property string itemName: ""

        function openFor(itemIds, name) {
            ids = itemIds.slice()
            itemName = name !== undefined ? name : ""
            open()
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: deleteDialog.ids.length === 1 && deleteDialog.itemName.length > 0
                  ? "« " + deleteDialog.itemName + " » sera retirée de la liste, avec "
                    + "ses étapes, pour tout le monde."
                  : deleteDialog.ids.length + (deleteDialog.ids.length > 1
                        ? " tâches seront retirées de la liste, avec leurs étapes, pour tout le monde."
                        : " tâche sera retirée de la liste, avec ses étapes, pour tout le monde.")
        }

        onAccepted: {
            AppController.items.removeItems(deleteDialog.ids)
            root.selectedIds = []
        }
    }

    ColoDialog {
        id: renameDialog
        title: "Renommer la liste"
        acceptText: "Renommer"
        acceptEnabled: renameField.text.trim().length > 0

        ColoTextField {
            id: renameField
            Layout.fillWidth: true
            hint: "Nom de la liste"
            onAccepted: if (renameDialog.acceptEnabled) renameDialog.accept()
        }

        onOpened: {
            renameField.text = root.listTitle
            renameField.forceActiveFocus()
            renameField.selectAll()
        }
        onAccepted: AppController.renameList(root.listId, renameField.text.trim())
    }

    ColoDialog {
        id: duplicateDialog
        title: "Dupliquer la liste"
        acceptText: "Dupliquer"
        acceptEnabled: duplicateField.text.trim().length > 0

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 13
            text: "Une nouvelle liste, avec les mêmes tâches et leurs étapes, toutes "
                  + "à faire. Elle est indépendante : la partager demande un nouveau lien."
        }

        ColoTextField {
            id: duplicateField
            Layout.fillWidth: true
            hint: "Nom de la copie"
            onAccepted: if (duplicateDialog.acceptEnabled) duplicateDialog.accept()
        }

        onOpened: {
            duplicateField.text = root.listTitle + " (copie)"
            duplicateField.forceActiveFocus()
            duplicateField.selectAll()
        }
        onAccepted: AppController.duplicateList(root.listId, duplicateField.text.trim())
    }
}
