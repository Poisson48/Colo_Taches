import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: root

    readonly property string pageTitle: "Mes listes"

    // Vrai le temps qu'une poignée de déplacement est tenue : fige le défilement des
    // listes pour que le glissement l'emporte sur écran tactile.
    property bool rowDragging: false

    // AppController.groups() lit la base à chaque appel ; rien ne le rappelle tout seul.
    // Le modèle des listes se réinitialise à chaque changement de groupe (reload en
    // C++) : on s'y raccroche pour rafraîchir la liste des groupes exposée à l'UI.
    property int groupsRev: 0
    readonly property var groupList: { groupsRev; return AppController.groups() }
    readonly property bool hasGroups: groupList.length > 0

    Connections {
        target: AppController.lists
        function onModelReset() { root.groupsRev++ }
    }

    function groupIdForName(name) {
        for (let i = 0; i < groupList.length; ++i)
            if (groupList[i].name === name)
                return groupList[i].id
        return ""
    }

    // Boutons affichés dans la barre supérieure par Main.qml.
    property Component actions: Row {
        spacing: 0

        ToolButton {
            width: 96
            height: Theme.touchTarget
            contentItem: Label {
                text: "Rejoindre"
                color: Theme.accent
                font.pixelSize: 14
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: joinDialog.open()
        }

        // Qui je suis, en clair. Un « ⋮ » n'apprend à personne que son nom se change
        // là-dessous — et c'est ce nom que les autres voient sur chaque modification.
        ItemDelegate {
            id: profileButton
            width: Math.min(120, 44 + nameLabel.implicitWidth)
            height: Theme.touchTarget
            padding: 0

            background: Rectangle {
                radius: height / 2
                color: profileButton.pressed ? Theme.surfaceHigh : "transparent"
                border.color: Theme.outline
                border.width: 1
            }

            contentItem: Row {
                spacing: 6
                leftPadding: 6
                rightPadding: 10

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28; height: 28; radius: 14
                    color: Theme.accent

                    Label {
                        anchors.centerIn: parent
                        text: AppController.displayName.length > 0
                              ? AppController.displayName.charAt(0).toUpperCase() : "?"
                        color: Theme.onAccent
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                }

                Label {
                    id: nameLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: AppController.displayName
                    color: Theme.text
                    font.pixelSize: 14
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, 74)
                }
            }

            onClicked: nameDialog.open()
        }

        ToolButton {
            width: Theme.touchTarget
            height: Theme.touchTarget
            contentItem: Icon {
                name: "menu"
                color: Theme.text
                size: 18
            }
            onClicked: overflowMenu.popup()
        }
    }

    // Menu global : import / export, et gestion des catégories personnalisées.
    ColoMenu {
        id: overflowMenu

        MenuItem {
            text: "Notes de version"
            onTriggered: {
                const w = root.Window.window
                if (w && typeof w.openChangelog === "function")
                    w.openChangelog()
            }
        }
        MenuItem {
            text: "Synchronisation…"
            onTriggered: syncDialog.open()
        }
        MenuSeparator {}
        MenuItem {
            text: "Gérer les catégories"
            onTriggered: aislesDialog.open()
        }
        MenuSeparator {}
        MenuItem {
            text: "Importer une liste…"
            onTriggered: { const p = root.usePickers(); if (p) p.openImport() }
        }
        MenuItem {
            text: "Tout exporter (ZIP)"
            enabled: listView.count > 0
            onTriggered: { const p = root.usePickers(); if (p) p.openExportZip() }
        }
    }

    // Pas encore de nom choisi (installation neuve, ou mise à jour depuis une version
    // qui ne le demandait pas) : on le demande avant tout le reste.
    Component.onCompleted: {
        if (!AppController.hasDisplayName)
            nameDialog.open()
        else if (!AppController.notifOnboardingSeen)
            notifDialog.open()
    }

    ListView {
        id: listView
        anchors.fill: parent
        anchors.margins: Theme.gap
        topMargin: Theme.gap
        bottomMargin: 96          // laisse respirer le bouton flottant
        spacing: Theme.gap
        clip: true
        model: AppController.lists
        // La vue ne se fige QUE pendant qu'une poignée est tenue (sinon elle défile).
        interactive: !root.rowDragging

        // Sections par groupe. Les en-têtes n'apparaissent que si des groupes existent :
        // qui ne s'en sert pas voit la même liste plate qu'avant.
        section.property: "groupName"
        section.criteria: ViewSection.FullString
        section.delegate: ItemDelegate {
            id: header
            required property string section
            width: listView.width - 2 * Theme.gap
            height: root.hasGroups ? 40 : 0
            visible: height > 0
            padding: 0

            background: Item {}

            contentItem: RowLayout {
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    // Le groupe vide (« ») est réel : les listes non rangées.
                    text: header.section.length > 0 ? header.section : "Sans groupe"
                    color: Theme.accent
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    verticalAlignment: Text.AlignVCenter
                }

                // Renommer / supprimer : réservé aux vrais groupes, pas au fourre-tout.
                ToolButton {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    visible: header.section.length > 0
                    contentItem: Icon {
                        name: "menu"; color: Theme.textDim; size: 15
                    }
                    onClicked: {
                        groupMenu.groupName = header.section
                        groupMenu.groupId = root.groupIdForName(header.section)
                        groupMenu.popup()
                    }
                }
            }
        }

        // Conteneur immobile (place + zone de dépôt) contenant la carte, qui se
        // détache sous le doigt pendant un glissement.
        delegate: Item {
            id: wrapper
            width: listView.width - 2 * Theme.gap
            height: 96

            property int rowIndex: index
            // « Saisie » posée par la poignée à l'appui (fige la vue + détache la carte).
            property bool held: false
            z: held ? 2 : 1

            DropArea {
                anchors.fill: parent
                onEntered: function (drag) {
                    const source = drag.source
                    if (!source || source === wrapper)
                        return
                    // Franchir une frontière de groupe range la liste dans ce groupe :
                    // c'est le modèle qui en décide (moveList), pas la vue.
                    AppController.moveList(source.rowIndex, wrapper.rowIndex)
                }
            }

            ItemDelegate {
                id: card
                width: wrapper.width
                height: wrapper.height
                padding: 0

                Drag.active: wrapper.held
                Drag.source: wrapper
                Drag.hotSpot.x: width / 2
                Drag.hotSpot.y: height / 2
                opacity: wrapper.held ? 0.85 : 1.0
                states: State {
                    when: wrapper.held
                    ParentChange { target: card; parent: listView }
                }

                background: Rectangle {
                    radius: Theme.radius
                    color: card.pressed ? Theme.surfaceHigh : Theme.surface
                    border.color: Theme.outline
                    border.width: 1
                }

                contentItem: RowLayout {
                    spacing: Theme.gap
                    anchors.margins: Theme.pad

                    // Pastille du nombre de tâches restantes.
                    Rectangle {
                        Layout.leftMargin: Theme.pad
                        Layout.alignment: Qt.AlignVCenter
                        width: 44; height: 44
                        radius: 22
                        color: model.count > 0 ? Theme.accentDim : Theme.surfaceHigh

                        Label {
                            anchors.centerIn: parent
                            visible: model.count > 0
                            text: model.count
                            color: "#FFFFFF"
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                        }

                        Icon {
                            anchors.centerIn: parent
                            visible: model.count === 0
                            name: "check"
                            color: Theme.accent
                            size: 22
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 3

                        Label {
                            Layout.fillWidth: true
                            text: model.name
                            color: Theme.text
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            color: Theme.textDim
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            text: model.total === 0
                                  ? "Liste vide"
                                  : (model.count === 0
                                     ? "Tout est fait"
                                     : model.count + " à faire sur " + model.total)
                        }

                        // Avec qui la liste est partagée.
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 1
                            spacing: 5

                            Icon {
                                name: model.memberCount > 0 ? "check" : "close"
                                color: model.memberCount > 0 ? Theme.accent : Theme.textDim
                                size: 12
                                opacity: 0.9
                            }

                            Label {
                                Layout.fillWidth: true
                                color: Theme.textDim
                                font.pixelSize: 12
                                elide: Text.ElideRight
                                text: model.memberCount > 0
                                      ? "Partagée avec " + model.members
                                      : "Pas encore partagée"
                            }
                        }
                    }

                    ToolButton {
                        Layout.alignment: Qt.AlignVCenter
                        width: Theme.touchTarget
                        height: Theme.touchTarget
                        contentItem: Icon {
                            name: "menu"
                            color: Theme.textDim
                            size: 18
                        }
                        onClicked: {
                            cardMenu.listId = model.listId
                            cardMenu.listName = model.name
                            cardMenu.popup()
                        }
                    }

                    // Poignée de déplacement, toujours visible : on la glisse pour ranger
                    // la liste, sans menu. `held` fige la vue le temps du geste.
                    MouseArea {
                        id: listDragHandle
                        Layout.rightMargin: 6
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: Theme.touchTarget
                        drag.target: wrapper.held ? card : undefined
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

                onClicked: AppController.openList(model.listId)
            }
        }
    }

    // État vide : c'est le premier écran d'un nouvel utilisateur, il doit dire quoi faire.
    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 80
        spacing: Theme.gap
        visible: listView.count === 0

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 88; height: 88; radius: 44
            color: Theme.surface
            border.color: Theme.outline
            border.width: 1

            Label {
                anchors.centerIn: parent
                text: "+"
                color: Theme.accent
                font.pixelSize: 40
            }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "Aucune liste"
            color: Theme.text
            font.pixelSize: 19
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Créez une liste, puis partagez-la pour que vos tâches restent synchronisées."
            color: Theme.textDim
            font.pixelSize: 14
        }
    }

    // Bouton flottant : créer une liste.
    RoundButton {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        width: 60; height: 60
        Material.background: Theme.accent
        Material.elevation: 6

        contentItem: Label {
            text: "+"
            color: Theme.onAccent
            font.pixelSize: 28
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        onClicked: createDialog.open()
    }

    ColoMenu {
        id: cardMenu
        property string listId: ""
        property string listName: ""

        MenuItem {
            text: "Partager la liste"
            onTriggered: shareSheet.openFor(cardMenu.listId, cardMenu.listName)
        }
        MenuItem {
            text: "Renommer la liste"
            onTriggered: renameDialog.openFor(cardMenu.listId, cardMenu.listName)
        }
        MenuItem {
            text: "Dupliquer la liste"
            onTriggered: duplicateDialog.openFor(cardMenu.listId, cardMenu.listName)
        }
        MenuItem {
            text: "Importer une liste ici…"
            onTriggered: importPicker.openFor(cardMenu.listId, cardMenu.listName)
        }
        MenuItem {
            text: "Ranger dans un groupe"
            onTriggered: groupPicker.openFor(cardMenu.listId)
        }
        MenuItem {
            text: "Exporter en CSV"
            onTriggered: { const p = root.usePickers(); if (p) p.openExportCsv(cardMenu.listId) }
        }
        MenuItem {
            text: "Quitter la liste"
            onTriggered: {
                leaveDialog.listId = cardMenu.listId
                leaveDialog.listName = cardMenu.listName
                leaveDialog.open()
            }
        }
    }

    // Menu d'un en-tête de groupe : renommer, ou supprimer (les listes sont conservées).
    ColoMenu {
        id: groupMenu
        property string groupId: ""
        property string groupName: ""

        MenuItem {
            text: "Renommer le groupe"
            onTriggered: groupRenameDialog.openFor(groupMenu.groupId, groupMenu.groupName)
        }
        MenuItem {
            text: "Supprimer le groupe"
            onTriggered: AppController.deleteGroup(groupMenu.groupId)
        }
    }

    ShareSheet { id: shareSheet }

    // Sélecteurs de fichiers natifs, isolés dans leur propre fichier : ils dépendent de
    // QtQuick.Dialogs, absent de certains kits Qt de bureau. Chargés seulement à la
    // demande (active au premier usage), l'écran des listes reste affichable sans ce
    // module. item null = module indisponible → on le dit plutôt que de planter.
    Loader {
        id: pickers
        active: false
        source: "FilePickers.qml"
    }

    function usePickers() {
        pickers.active = true
        if (!pickers.item)
            AppController.toast("Sélecteur de fichiers indisponible sur cet appareil")
        return pickers.item
    }

    // --- Dialogues ---

    // Choisir le groupe d'une liste : un groupe existant, un nouveau, ou aucun.
    ColoDialog {
        id: groupPicker
        title: "Ranger dans un groupe"
        // Pas de bouton de validation : chaque ligne agit au clic.
        showAccept: false

        property string listId: ""

        function openFor(id) {
            listId = id
            open()
        }

        Repeater {
            model: root.groupList
            delegate: Button {
                required property var modelData
                Layout.fillWidth: true
                flat: true
                implicitHeight: 46
                contentItem: Label {
                    text: modelData.name
                    color: Theme.text
                    font.pixelSize: 15
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 10
                    color: parent.pressed ? Theme.surfaceHigh : "transparent"
                }
                onClicked: {
                    AppController.setListGroup(groupPicker.listId, modelData.id)
                    groupPicker.close()
                }
            }
        }

        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 46
            contentItem: Label {
                text: "＋ Nouveau groupe…"
                color: Theme.accent
                font.pixelSize: 15
                font.weight: Font.DemiBold
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 10
                color: parent.pressed ? Theme.surfaceHigh : "transparent"
            }
            onClicked: {
                groupPicker.close()
                newGroupDialog.openFor(groupPicker.listId)
            }
        }

        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 46
            visible: root.hasGroups
            contentItem: Label {
                text: "Retirer du groupe"
                color: Theme.textDim
                font.pixelSize: 15
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 10
                color: parent.pressed ? Theme.surfaceHigh : "transparent"
            }
            onClicked: {
                AppController.setListGroup(groupPicker.listId, "")
                groupPicker.close()
            }
        }
    }

    // Importer le contenu d'une autre liste dans celle-ci : choisir la liste source,
    // ses tâches sont recopiées « à faire ». La source reste intacte (liste-modèle).
    ColoDialog {
        id: importPicker
        title: "Importer une liste ici"
        // Pas de bouton de validation : chaque ligne agit au clic.
        showAccept: false

        property string destId: ""
        property string destName: ""
        property var sources: []

        function openFor(id, name) {
            destId = id
            destName = name
            sources = AppController.otherLists(id)
            open()
        }

        Label {
            Layout.fillWidth: true
            visible: importPicker.sources.length > 0
            text: "Ses tâches seront ajoutées à « " + importPicker.destName + " », à faire."
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 13
        }

        Label {
            Layout.fillWidth: true
            visible: importPicker.sources.length === 0
            text: "Aucune autre liste à importer."
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 15
        }

        Repeater {
            model: importPicker.sources
            delegate: Button {
                required property var modelData
                Layout.fillWidth: true
                flat: true
                implicitHeight: 46
                contentItem: Label {
                    text: modelData.name
                    color: Theme.text
                    font.pixelSize: 15
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 10
                    color: parent.pressed ? Theme.surfaceHigh : "transparent"
                }
                onClicked: {
                    AppController.importListInto(importPicker.destId, modelData.id)
                    importPicker.close()
                }
            }
        }
    }

    // Créer un groupe, et y ranger la liste dans la foulée.
    ColoDialog {
        id: newGroupDialog
        title: "Nouveau groupe"
        acceptText: "Créer"
        acceptEnabled: newGroupField.text.trim().length > 0

        property string listId: ""

        function openFor(id) {
            listId = id
            open()
            newGroupField.text = ""
            newGroupField.forceActiveFocus()
        }

        ColoTextField {
            id: newGroupField
            Layout.fillWidth: true
            hint: "Maison, Boulot, Vacances…"
            onAccepted: if (newGroupDialog.acceptEnabled) newGroupDialog.accept()
        }

        onAccepted: {
            const gid = AppController.createGroup(newGroupField.text.trim())
            if (gid.length > 0 && newGroupDialog.listId.length > 0)
                AppController.setListGroup(newGroupDialog.listId, gid)
        }
    }

    ColoDialog {
        id: groupRenameDialog
        title: "Renommer le groupe"
        acceptText: "Renommer"
        acceptEnabled: groupRenameField.text.trim().length > 0

        property string groupId: ""

        function openFor(id, currentName) {
            groupId = id
            open()
            groupRenameField.text = currentName
            groupRenameField.forceActiveFocus()
            groupRenameField.selectAll()
        }

        ColoTextField {
            id: groupRenameField
            Layout.fillWidth: true
            hint: "Nom du groupe"
            onAccepted: if (groupRenameDialog.acceptEnabled) groupRenameDialog.accept()
        }

        onAccepted: AppController.renameGroup(groupRenameDialog.groupId,
                                              groupRenameField.text.trim())
    }

    ColoDialog {
        id: createDialog
        title: "Nouvelle liste"
        acceptText: "Créer"
        acceptEnabled: nameField.text.trim().length > 0

        ColoTextField {
            id: nameField
            Layout.fillWidth: true
            hint: "Corvées de la coloc"
            onAccepted: if (createDialog.acceptEnabled) createDialog.accept()
        }

        onOpened: { nameField.text = ""; nameField.forceActiveFocus() }
        onAccepted: AppController.createList(nameField.text.trim())
    }

    // Scanner : plein écran, par-dessus tout le reste. Chargé seulement à l'ouverture —
    // instancié d'emblée, il imposerait QtMultimedia au chargement de cette page, et
    // un desktop sans ce module perdrait l'écran des listes entier.
    Popup {
        id: scanPopup
        parent: Overlay.overlay
        width: parent.width
        height: parent.height
        padding: 0
        modal: true
        closePolicy: Popup.CloseOnEscape
        background: Rectangle { color: "black" }

        Loader {
            id: scanLoader
            anchors.fill: parent
            source: scanPopup.opened ? "ScanPage.qml" : ""

            onStatusChanged: {
                if (status === Loader.Error) {
                    scanPopup.close()
                    AppController.toast("Caméra indisponible sur cet appareil")
                }
            }
        }

        Connections {
            target: scanLoader.item
            ignoreUnknownSignals: true
            function onJoined() { scanPopup.close() }
            function onCloseRequested() { scanPopup.close() }
        }
    }

    ColoDialog {
        id: joinDialog
        title: "Rejoindre une liste"
        acceptText: "Rejoindre"
        acceptEnabled: uriField.text.trim().length > 0

        Button {
            Layout.fillWidth: true
            implicitHeight: 52
            text: "Scanner le QR code"
            background: Rectangle {
                radius: 12
                color: parent.pressed ? Theme.accentDim : Theme.accent
            }
            contentItem: Label {
                text: parent.text
                color: Theme.onAccent
                font.pixelSize: 15
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: { joinDialog.close(); scanPopup.open() }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "ou collez le lien reçu"
            color: Theme.textDim
            font.pixelSize: 13
        }

        ColoTextField {
            id: uriField
            Layout.fillWidth: true
            hint: "colotaches://join/1/…"
            onAccepted: if (joinDialog.acceptEnabled) joinDialog.accept()
        }

        onOpened: { uriField.text = ""; uriField.forceActiveFocus() }
        onAccepted: {
            if (!AppController.joinList(uriField.text.trim()))
                AppController.toast("Lien d'invitation invalide")
        }
    }

    ColoDialog {
        id: renameDialog
        title: "Renommer la liste"
        acceptText: "Renommer"
        acceptEnabled: renameField.text.trim().length > 0

        property string listId: ""

        function openFor(id, currentName) {
            listId = id
            open()
            renameField.text = currentName
            renameField.forceActiveFocus()
            renameField.selectAll()
        }

        ColoTextField {
            id: renameField
            Layout.fillWidth: true
            hint: "Nom de la liste"
            onAccepted: if (renameDialog.acceptEnabled) renameDialog.accept()
        }

        onAccepted: AppController.renameList(renameDialog.listId, renameField.text.trim())
    }

    ColoDialog {
        id: duplicateDialog
        title: "Dupliquer la liste"
        acceptText: "Dupliquer"
        acceptEnabled: duplicateField.text.trim().length > 0

        property string listId: ""

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 13
            text: "Une nouvelle liste, avec les mêmes tâches et leurs étapes, toutes à faire. "
                  + "Elle est indépendante : la partager demande un nouveau lien."
        }

        ColoTextField {
            id: duplicateField
            Layout.fillWidth: true
            hint: "Nom de la copie"
            onAccepted: if (duplicateDialog.acceptEnabled) duplicateDialog.accept()
        }

        function openFor(id, currentName) {
            listId = id
            open()
            duplicateField.text = currentName + " (copie)"
            duplicateField.forceActiveFocus()
            duplicateField.selectAll()
        }

        onAccepted: AppController.duplicateList(duplicateDialog.listId,
                                                duplicateField.text.trim())
    }

    ColoDialog {
        id: nameDialog
        title: AppController.hasDisplayName ? "Mon nom" : "Comment vous appelez-vous ?"
        acceptText: "Enregistrer"
        acceptEnabled: displayNameField.text.trim().length > 0

        // Au premier lancement, ce nom n'a pas encore été choisi : tant qu'il vaut
        // « Moi », les autres participants reçoivent « 2 tâches modifiées par Moi ».
        // On le demande donc d'emblée, et on ne laisse pas esquiver d'un appui à côté.
        closePolicy: AppController.hasDisplayName
                     ? (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
                     : Popup.NoAutoClose

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Le nom que voient les autres participants : « 3 tâches ajoutées par… »"
            color: Theme.textDim
            font.pixelSize: 13
        }

        ColoTextField {
            id: displayNameField
            Layout.fillWidth: true
            hint: "Votre prénom"
            onAccepted: if (nameDialog.acceptEnabled) nameDialog.accept()
        }

        onOpened: {
            // Premier lancement : champ vide plutôt que « Moi » prérempli, qu'on
            // validerait sans y penser — et tout le monde s'appellerait « Moi ».
            displayNameField.text = AppController.hasDisplayName
                                    ? AppController.displayName : ""
            displayNameField.forceActiveFocus()
            displayNameField.selectAll()
        }
        onAccepted: {
            AppController.displayName = displayNameField.text.trim()
            if (!AppController.notifOnboardingSeen)
                Qt.callLater(notifDialog.open)
        }
    }

    // Choix des notifications : proposé une seule fois, sans présélection.
    ColoDialog {
        id: notifDialog
        title: "Notifications"
        acceptText: "Activer"
        acceptEnabled: notifChoice.length > 0
        cancelText: "Plus tard"

        property string notifChoice: ""
        property int notifInterval: 15
        property string notifSignal: "sound"
        property string notifProviderChoice: "fcm"

        onOpened: {
            notifChoice = ""
            notifInterval = 15
            notifSignal = AppController.notifSignal
            notifProviderChoice = AppController.notifProvider === "unifiedpush" || !AppController.notifImmediateAvailable ? "unifiedpush" : "fcm"
            AppController.markNotificationOnboardingSeen()
        }
        onAccepted: {
            const notify = notifChoice !== "silent" && notifChoice !== "off"
            AppController.setNotificationPreferences(notify ? "notify" : "silent", notifSignal)
            if (notifChoice === "off") AppController.setNotificationMode("off", "auto", 15)
            else if (notifChoice === "silent") AppController.setNotificationMode("deferred", "auto", notifInterval)
            else if (notifChoice === "deferred") AppController.setNotificationMode("deferred", "auto", notifInterval)
            else AppController.setNotificationMode("immediate", notifProviderChoice, notifInterval)
        }

        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 13; text: "Immédiat : Google FCM ou UnifiedPush. Par périodes : sans Google, via le serveur Colo." }
        ButtonGroup { id: notifGroupBox }
        RadioButton { text: "M'avertir immédiatement"; checked: notifDialog.notifChoice === "immediate"; onCheckedChanged: if (checked) notifDialog.notifChoice = "immediate"; ButtonGroup.group: notifGroupBox }
        RadioButton { text: "M'avertir par périodes (sans Google)"; checked: notifDialog.notifChoice === "deferred"; onCheckedChanged: if (checked) notifDialog.notifChoice = "deferred"; ButtonGroup.group: notifGroupBox }
        RadioButton { text: "Synchroniser sans notification"; checked: notifDialog.notifChoice === "silent"; onCheckedChanged: if (checked) notifDialog.notifChoice = "silent"; ButtonGroup.group: notifGroupBox }
        RadioButton { text: "Ne rien faire en arrière-plan"; checked: notifDialog.notifChoice === "off"; onCheckedChanged: if (checked) notifDialog.notifChoice = "off"; ButtonGroup.group: notifGroupBox }
        RowLayout {
            Layout.fillWidth: true
            visible: notifDialog.notifChoice === "immediate"
            Label { text: "Moteur"; color: Theme.textDim; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            ComboBox {
                model: ["Google FCM", "Sans Google (UnifiedPush)"]
                currentIndex: notifDialog.notifProviderChoice === "unifiedpush" ? 1 : 0
                onActivated: notifDialog.notifProviderChoice = index === 1 ? "unifiedpush" : "fcm"
                font.pixelSize: 14
            }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 11; visible: notifDialog.notifChoice === "immediate"; text: notifDialog.notifProviderChoice === "fcm" ? "Google reçoit un signal générique, jamais le contenu des listes." : "Passe par ton distributeur UnifiedPush (ntfy), sans Google." }
        RowLayout {
            Layout.fillWidth: true
            visible: notifDialog.notifChoice === "deferred"
            Label { text: "Toutes les"; color: Theme.textDim; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            ComboBox {
                model: ["15 min", "30 min", "60 min"]
                currentIndex: notifDialog.notifInterval === 30 ? 1 : notifDialog.notifInterval === 60 ? 2 : 0
                onActivated: notifDialog.notifInterval = index === 1 ? 30 : (index === 2 ? 60 : 15)
                font.pixelSize: 14
            }
        }
        Label { Layout.fillWidth: true; Layout.topMargin: 4; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 13; visible: notifDialog.notifChoice !== "silent" && notifDialog.notifChoice !== "off"; text: "Quand une notification s'affiche :" }
        RowLayout {
            Layout.fillWidth: true
            visible: notifDialog.notifChoice !== "silent" && notifDialog.notifChoice !== "off"
            Label { text: "Signal"; color: Theme.textDim; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            ComboBox {
                model: ["Son", "Vibration", "Visuel seulement"]
                currentIndex: notifDialog.notifSignal === "vibrate" ? 1 : notifDialog.notifSignal === "visual" ? 2 : 0
                onActivated: notifDialog.notifSignal = index === 1 ? "vibrate" : (index === 2 ? "visual" : "sound")
                font.pixelSize: 14
            }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 12; visible: notifDialog.notifChoice === "silent"; text: "Les listes continueront à se synchroniser en arrière-plan, sans aucune notification." }

    }

    ColoDialog {
        id: notifPrefsDialog
        title: "Notifications"
        acceptText: "Enregistrer"
        cancelText: "Annuler"

        property string notifChoice: ""
        property int notifInterval: 15
        property string notifSignal: "sound"
        property string notifProviderChoice: "fcm"

        onOpened: {
            const m = AppController.notifMode
            const a = AppController.notifAlert
            notifChoice = m === "off" ? "off" : (a === "silent" ? "silent" : (m === "deferred" ? "deferred" : "immediate"))
            notifInterval = AppController.notifInterval
            notifSignal = AppController.notifSignal
            notifProviderChoice = AppController.notifProvider === "unifiedpush" || !AppController.notifImmediateAvailable ? "unifiedpush" : "fcm"
        }
        onAccepted: {
            const notify = notifChoice !== "silent" && notifChoice !== "off"
            AppController.setNotificationPreferences(notify ? "notify" : "silent", notifSignal)
            if (notifChoice === "off") AppController.setNotificationMode("off", "auto", 15)
            else if (notifChoice === "silent") AppController.setNotificationMode("deferred", "auto", notifInterval)
            else if (notifChoice === "deferred") AppController.setNotificationMode("deferred", "auto", notifInterval)
            else AppController.setNotificationMode("immediate", notifProviderChoice, notifInterval)
        }

        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 13; text: "Immédiat : Google FCM ou UnifiedPush. Par périodes : sans Google, via le serveur Colo." }
        ButtonGroup { id: prefsGroupBox }
        RadioButton { text: "M'avertir immédiatement"; checked: notifPrefsDialog.notifChoice === "immediate"; onCheckedChanged: if (checked) notifPrefsDialog.notifChoice = "immediate"; ButtonGroup.group: prefsGroupBox }
        RadioButton { text: "M'avertir par périodes (sans Google)"; checked: notifPrefsDialog.notifChoice === "deferred"; onCheckedChanged: if (checked) notifPrefsDialog.notifChoice = "deferred"; ButtonGroup.group: prefsGroupBox }
        RadioButton { text: "Synchroniser sans notification"; checked: notifPrefsDialog.notifChoice === "silent"; onCheckedChanged: if (checked) notifPrefsDialog.notifChoice = "silent"; ButtonGroup.group: prefsGroupBox }
        RadioButton { text: "Ne rien faire en arrière-plan"; checked: notifPrefsDialog.notifChoice === "off"; onCheckedChanged: if (checked) notifPrefsDialog.notifChoice = "off"; ButtonGroup.group: prefsGroupBox }
        RowLayout {
            Layout.fillWidth: true
            visible: notifPrefsDialog.notifChoice === "immediate"
            Label { text: "Moteur"; color: Theme.textDim; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            ComboBox {
                model: ["Google FCM", "Sans Google (UnifiedPush)"]
                currentIndex: notifPrefsDialog.notifProviderChoice === "unifiedpush" ? 1 : 0
                onActivated: notifPrefsDialog.notifProviderChoice = index === 1 ? "unifiedpush" : "fcm"
                font.pixelSize: 14
            }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 11; visible: notifPrefsDialog.notifChoice === "immediate"; text: notifPrefsDialog.notifProviderChoice === "fcm" ? "Google reçoit un signal générique, jamais le contenu des listes." : "Passe par ton distributeur UnifiedPush (ntfy), sans Google." }
        RowLayout {
            Layout.fillWidth: true
            visible: notifPrefsDialog.notifChoice === "deferred"
            Label { text: "Toutes les"; color: Theme.textDim; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            ComboBox {
                model: ["15 min", "30 min", "60 min"]
                currentIndex: notifPrefsDialog.notifInterval === 30 ? 1 : notifPrefsDialog.notifInterval === 60 ? 2 : 0
                onActivated: notifPrefsDialog.notifInterval = index === 1 ? 30 : (index === 2 ? 60 : 15)
                font.pixelSize: 14
            }
        }
        Label { Layout.fillWidth: true; Layout.topMargin: 4; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 13; visible: notifPrefsDialog.notifChoice !== "silent" && notifPrefsDialog.notifChoice !== "off"; text: "Quand une notification s'affiche :" }
        RowLayout {
            Layout.fillWidth: true
            visible: notifPrefsDialog.notifChoice !== "silent" && notifPrefsDialog.notifChoice !== "off"
            Label { text: "Signal"; color: Theme.textDim; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            ComboBox {
                model: ["Son", "Vibration", "Visuel seulement"]
                currentIndex: notifPrefsDialog.notifSignal === "vibrate" ? 1 : notifPrefsDialog.notifSignal === "visual" ? 2 : 0
                onActivated: notifPrefsDialog.notifSignal = index === 1 ? "vibrate" : (index === 2 ? "visual" : "sound")
                font.pixelSize: 14
            }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 12; visible: notifPrefsDialog.notifChoice === "silent"; text: "Les listes continueront à se synchroniser en arrière-plan, sans aucune notification." }

    }

    ColoDialog {
        id: syncDialog
        title: "Réglages"
        acceptText: "Enregistrer"
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 13; text: "Synchronisation sécurisée entre tes appareils." }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 12; text: "Serveur (adresse technique, une URL par ligne) :" }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            radius: 12
            color: Theme.surfaceHigh
            border.color: Theme.outline

            TextArea {
                id: relayField
                anchors.fill: parent
                anchors.margins: 8
                wrapMode: TextArea.Wrap
                color: Theme.text
                font.pixelSize: 13
                font.family: "monospace"
                selectByMouse: true
                background: null
                placeholderText: AppController.defaultRelayUrls()
            }
        }

        Label { Layout.fillWidth: true; Layout.topMargin: 8; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: 13; text: "Notifications" }
        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 40
            contentItem: Label { text: "Choisir comment être prévenu…"; color: Theme.accent; horizontalAlignment: Text.AlignHCenter }
            onClicked: {
                syncDialog.close()
                notifPrefsDialog.open()
            }
        }
        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 40
            contentItem: Label { text: "Tester la notification"; color: Theme.accent; horizontalAlignment: Text.AlignHCenter }
            onClicked: AppController.testNotification()
        }

        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 40
            contentItem: Label {
                text: "Revoir le choix des notifications"
                color: Theme.textDim
                horizontalAlignment: Text.AlignHCenter
            }
            onClicked: {
                syncDialog.close()
                AppController.resetNotificationOnboarding()
                notifDialog.open()
            }
        }

        onOpened: relayField.text = AppController.relayUrls
        onAccepted: {
            AppController.setRelayUrls(relayField.text)
            AppController.setSyncMode(0)
        }
    }

    // --- Gestion des catégories personnalisées ---

    ColoDialog {
        id: aislesDialog
        objectName: "aislesDialog"
        title: "Catégories personnalisées"
        acceptText: "Fermer"
        // Les lignes agissent d'elles-mêmes ; un seul bouton « Fermer » suffit.
        showCancel: false
        onAccepted: {}

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 13
            visible: AppController.customAisles.length === 0
            text: "Aucune catégorie personnalisée. Celles que vous créez en classant vos tâches "
                  + "apparaîtront ici, à renommer ou supprimer."
        }

        Repeater {
            model: AppController.customAisles
            delegate: RowLayout {
                required property string modelData
                Layout.fillWidth: true
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    text: modelData
                    color: Theme.text
                    font.pixelSize: 15
                    elide: Text.ElideRight
                }

                ToolButton {
                    Layout.preferredWidth: 84
                    Layout.preferredHeight: Theme.touchTarget
                    contentItem: Label {
                        text: "Renommer"
                        color: Theme.accent
                        font.pixelSize: 13
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: aisleRenameDialog.openFor(modelData)
                }
                ToolButton {
                    Layout.preferredWidth: Theme.touchTarget
                    Layout.preferredHeight: Theme.touchTarget
                    contentItem: Icon { name: "close"; color: Theme.danger; size: 15 }
                    onClicked: aisleDeleteDialog.openFor(modelData)
                }
            }
        }
    }

    ColoDialog {
        id: aisleRenameDialog
        title: "Renommer la catégorie"
        acceptText: "Renommer"
        acceptEnabled: aisleRenameField.text.trim().length > 0

        property string aisle: ""

        function openFor(a) {
            aisle = a
            open()
            aisleRenameField.text = a
            aisleRenameField.forceActiveFocus()
            aisleRenameField.selectAll()
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 13
            text: "Les tâches classées dans cette catégorie suivront, sur tous les appareils."
        }

        ColoTextField {
            id: aisleRenameField
            Layout.fillWidth: true
            hint: "Nom de la catégorie"
            onAccepted: if (aisleRenameDialog.acceptEnabled) aisleRenameDialog.accept()
        }

        onAccepted: AppController.renameAisle(aisleRenameDialog.aisle,
                                              aisleRenameField.text.trim())
    }

    ColoDialog {
        id: aisleDeleteDialog
        title: "Supprimer la catégorie ?"
        acceptText: "Supprimer"
        destructive: true

        property string aisle: ""
        property int    count: 0

        function openFor(a) {
            aisle = a
            count = AppController.countItemsInAisle(a)
            open()
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: aisleDeleteDialog.count > 0
                  ? "« " + aisleDeleteDialog.aisle + " » sera retiré de "
                    + aisleDeleteDialog.count + " tâche(s), qui repasseront « sans catégorie », "
                    + "et ne sera plus proposée. Les tâches ne sont pas supprimées."
                  : "« " + aisleDeleteDialog.aisle + " » ne sera plus proposé."
        }

        onAccepted: AppController.deleteAisle(aisleDeleteDialog.aisle)
    }
}
