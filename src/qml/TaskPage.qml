import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Page d'une tâche : cocher, décrire, illustrer d'une photo, et dérouler ses étapes.
// Les étapes vivent dans AppController.steps (chargé par openTask, avant le push).
Item {
    id: root

    required property string listId
    required property string itemId

    // Instantané de la tâche, relu à chaque changement du modèle (édition locale ou
    // merge distant) : la page suit ce que font les autres participants en direct.
    property var task: AppController.items.get(itemId)

    readonly property string pageTitle: task && task.name ? task.name : "Tâche"

    Connections {
        target: AppController.items
        function onRefreshed() {
            const t = AppController.items.get(root.itemId)
            if (!t || !t.itemId) {
                // La tâche vient d'être supprimée (ici ou sur un autre appareil) :
                // la page n'a plus d'objet.
                const view = root.StackView.view
                if (view)
                    view.pop()
                return
            }
            root.task = t
        }
    }

    // Débrancher le modèle d'étapes du SyncEngine quand la page se ferme.
    Component.onDestruction: AppController.closeTask()

    property Component actions: Row {
        spacing: 0

        ToolButton {
            width: Theme.touchTarget
            height: Theme.touchTarget
            contentItem: Icon {
                name: "menu"
                color: Theme.text
                size: 18
            }
            onClicked: taskMenu.popup()
        }
    }

    ColoMenu {
        id: taskMenu

        MenuItem {
            text: "Modifier la tâche"
            onTriggered: editDialog.openFor(root.task)
        }
        MenuItem {
            text: root.task && root.task.image && root.task.image.length > 0
                  ? "Remplacer la photo" : "Ajouter une photo"
            onTriggered: root.pickPhoto()
        }
        MenuItem {
            text: "Retirer la photo"
            enabled: root.task && root.task.image && root.task.image.length > 0
            onTriggered: AppController.removeTaskImage(root.itemId)
        }
        MenuSeparator {}
        MenuItem {
            text: "Supprimer la tâche"
            onTriggered: deleteDialog.open()
        }
    }

    // Sélecteur d'images du système, chargé au premier besoin seulement :
    // QtQuick.Dialogs peut manquer sur certains kits de bureau, et la page doit
    // vivre (et se charger sans bruit) sans lui.
    Loader {
        id: pickers
    }

    function pickPhoto() {
        if (!pickers.item && pickers.status !== Loader.Error)
            pickers.source = "FilePickers.qml"
        if (pickers.item)
            pickers.item.openPhoto(root.itemId)
        else
            AppController.toast("Sélecteur de fichiers indisponible")
    }

    // Une date lisible : « aujourd'hui à 18:32 », « hier à 09:05 », sinon « 3 juil. à 14:20 ».
    function formatStamp(ms) {
        if (!ms || ms <= 0)
            return ""
        const d = new Date(ms)
        const now = new Date()
        const sameDay = d.toDateString() === now.toDateString()
        const yesterday = new Date(now.getTime() - 86400000).toDateString() === d.toDateString()
        const time = Qt.formatDateTime(d, "HH:mm")
        if (sameDay)
            return "aujourd'hui à " + time
        if (yesterday)
            return "hier à " + time
        return Qt.formatDateTime(d, "d MMM") + " à " + time
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 2 * Theme.pad
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator {}

        ColumnLayout {
            id: content
            x: Theme.pad
            y: Theme.pad
            width: parent.width - 2 * Theme.pad
            spacing: Theme.gap

            // --- La tâche elle-même : cocher, lire, éditer d'un tap ---
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: headRow.implicitHeight + 20
                radius: 12
                color: Theme.surface
                border.color: Theme.outline
                border.width: 1

                RowLayout {
                    id: headRow
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    CheckBox {
                        Layout.alignment: Qt.AlignTop
                        implicitWidth: Theme.touchTarget
                        implicitHeight: Theme.touchTarget
                        checked: root.task && root.task.done === true
                        Material.accent: Theme.accent
                        onToggled: {
                            AppController.items.toggleDone(root.itemId)
                            AppController.vibrate()
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 3

                        Label {
                            Layout.fillWidth: true
                            text: root.task && root.task.name ? root.task.name : ""
                            color: root.task && root.task.done ? Theme.textDim : Theme.text
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            font.strikeout: root.task && root.task.done === true
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: {
                                if (!root.task) return ""
                                const parts = []
                                if (root.task.aisle && root.task.aisle.length > 0)
                                    parts.push(root.task.aisle)
                                const added = root.formatStamp(root.task.created)
                                if (added.length > 0)
                                    parts.push(root.task.byName && root.task.byName.length > 0
                                               ? "ajoutée par " + root.task.byName + " " + added
                                               : "ajoutée " + added)
                                if (root.task.doneAt > 0)
                                    parts.push("faite " + root.formatStamp(root.task.doneAt))
                                return parts.join(" · ")
                            }
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // Un tap hors de la case ouvre l'édition (nom, détails, catégorie).
                MouseArea {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.touchTarget + 10
                    onClicked: editDialog.openFor(root.task)
                }
            }

            // --- Détails (description libre) ---
            Rectangle {
                Layout.fillWidth: true
                visible: root.task && root.task.note && root.task.note.length > 0
                implicitHeight: noteLabel.implicitHeight + 24
                radius: 12
                color: Theme.surface
                border.color: Theme.outline
                border.width: 1

                Label {
                    id: noteLabel
                    anchors.fill: parent
                    anchors.margins: 12
                    text: root.task && root.task.note ? root.task.note : ""
                    color: Theme.text
                    font.pixelSize: 14
                    wrapMode: Text.WordWrap
                }
            }

            // --- Photo ---
            Rectangle {
                Layout.fillWidth: true
                visible: root.task && root.task.image && root.task.image.length > 0
                implicitHeight: photo.status === Image.Ready
                                ? photo.paintedHeight + 2 : 120
                radius: 12
                clip: true
                color: Theme.surfaceHigh
                border.color: Theme.outline
                border.width: 1

                Image {
                    id: photo
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 1
                    width: parent.width - 2
                    source: root.task && root.task.image && root.task.image.length > 0
                            ? "image://taskimg/" + root.task.image
                              + "?r=" + AppController.imageRevision
                            : ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }

                // Blob pas encore reçu d'un relais : le dire, plutôt qu'un cadre vide.
                Label {
                    anchors.centerIn: parent
                    visible: photo.status !== Image.Ready
                    text: "Photo en cours de réception…"
                    color: Theme.textDim
                    font.pixelSize: 13
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: if (photo.status === Image.Ready) photoViewer.open()
                }
            }

            // Pas encore de photo : proposer d'en mettre une, sans passer par le menu.
            Button {
                Layout.fillWidth: true
                visible: !(root.task && root.task.image && root.task.image.length > 0)
                flat: true
                implicitHeight: Theme.touchTarget
                contentItem: Label {
                    text: "＋  Ajouter une photo"
                    color: Theme.accent
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 10
                    color: parent.pressed ? Theme.surfaceHigh : "transparent"
                    border.color: Theme.outline
                    border.width: 1
                }
                onClicked: root.pickPhoto()
            }

            // --- Étapes ---
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8

                Label {
                    text: "Étapes"
                    color: Theme.accent
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                }

                Label {
                    visible: AppController.steps.count > 0
                    text: AppController.steps.doneCount + "/" + AppController.steps.count
                    color: AppController.steps.count > 0
                           && AppController.steps.doneCount === AppController.steps.count
                           ? Theme.accent : Theme.textDim
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }

                Item { Layout.fillWidth: true }
            }

            // Barre de progression des étapes : le coup d'œil suffit.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 4
                visible: AppController.steps.count > 0
                radius: 2
                color: Theme.surfaceHigh

                Rectangle {
                    height: parent.height
                    radius: 2
                    color: Theme.accent
                    width: AppController.steps.count > 0
                           ? parent.width * (AppController.steps.doneCount
                                             / AppController.steps.count)
                           : 0
                    Behavior on width { NumberAnimation { duration: 180 } }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: AppController.steps.count === 0
                text: "Découpez la tâche en étapes à cocher — chacun voit où elle en est."
                color: Theme.textDim
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }

            // Les étapes gardent leur place une fois cochées (tri du modèle) : la
            // séquence « démonter, poncer, repeindre » reste lisible telle quelle.
            Repeater {
                model: AppController.steps

                delegate: Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 52
                    radius: 12
                    color: Theme.surface
                    border.color: Theme.outline
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        spacing: 0

                        CheckBox {
                            Layout.leftMargin: 6
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: Theme.touchTarget
                            implicitHeight: Theme.touchTarget
                            checked: model.done
                            Material.accent: Theme.accent
                            onToggled: {
                                AppController.steps.toggleDone(model.itemId)
                                AppController.vibrate()
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 4
                            text: model.name
                            color: model.done ? Theme.textDim : Theme.text
                            font.pixelSize: 15
                            font.strikeout: model.done
                            elide: Text.ElideRight
                        }

                        ToolButton {
                            Layout.rightMargin: 2
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 38
                            implicitHeight: Theme.touchTarget
                            contentItem: Icon {
                                name: "close"
                                color: Theme.textDim
                                size: 14
                            }
                            onClicked: AppController.steps.removeItem(model.itemId)
                        }
                    }
                }
            }

            // Ajout d'étape, sous la dernière : le flux naturel d'un découpage.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ColoTextField {
                    id: stepField
                    Layout.fillWidth: true
                    hint: "Ajouter une étape"
                    onAccepted: root.addStep()
                }

                RoundButton {
                    id: addStepButton
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 44
                    enabled: stepField.text.trim().length > 0

                    background: Rectangle {
                        radius: width / 2
                        color: !addStepButton.enabled ? Theme.surfaceHigh
                             : (addStepButton.pressed ? Theme.accentDim : Theme.accent)
                    }

                    contentItem: Label {
                        text: "+"
                        color: addStepButton.enabled ? Theme.onAccent : Theme.textDim
                        font.pixelSize: 20
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    onClicked: root.addStep()
                }
            }

            Item { Layout.preferredHeight: 8 }
        }
    }

    function addStep() {
        const name = stepField.text.trim()
        if (name.length === 0)
            return
        AppController.steps.addItem(name, "", "", "")
        stepField.text = ""
        stepField.forceActiveFocus()
    }

    // --- Photo en plein écran ---
    Popup {
        id: photoViewer
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: parent.width
        height: parent.height
        padding: 0
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: Qt.rgba(0, 0, 0, 0.92) }

        contentItem: Item {
            Image {
                anchors.fill: parent
                anchors.margins: 8
                source: photo.source
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }

            // Tout l'écran ferme : le geste évident devant une photo en grand.
            MouseArea {
                anchors.fill: parent
                onClicked: photoViewer.close()
            }
        }
    }

    // --- Dialogues ---

    ColoDialog {
        id: editDialog
        title: "Modifier la tâche"
        acceptText: "Enregistrer"
        acceptEnabled: editName.text.trim().length > 0

        function openFor(task) {
            if (!task) return
            editName.text = task.name || ""
            editNote.text = task.note || ""
            editAisle.aisle = task.aisle || ""
            open()
            editName.forceActiveFocus()
            editName.selectAll()
        }

        ColoTextField {
            id: editName
            Layout.fillWidth: true
            hint: "Tâche"
            onAccepted: if (editDialog.acceptEnabled) editDialog.accept()
        }

        ColoTextField {
            id: editNote
            Layout.fillWidth: true
            hint: "Détails"
            onAccepted: if (editDialog.acceptEnabled) editDialog.accept()
        }

        AisleBox {
            id: editAisle
            Layout.fillWidth: true
        }

        onAccepted: AppController.items.editItem(root.itemId,
                                                 editName.text.trim(),
                                                 "",
                                                 editNote.text.trim(),
                                                 editAisle.aisle)
    }

    ColoDialog {
        id: deleteDialog
        title: "Supprimer ?"
        acceptText: "Supprimer"
        destructive: true

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: "« " + (root.task && root.task.name ? root.task.name : "") + " » sera "
                  + "retirée de la liste, avec ses étapes, pour tout le monde."
        }

        // La suppression retire la ligne du modèle : onRefreshed voit la tâche
        // disparue et referme la page.
        onAccepted: AppController.items.removeItem(root.itemId)
    }
}
