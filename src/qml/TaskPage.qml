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

    // Une poignée d'étape est tenue : la page ne défile plus, le geste réordonne.
    property bool stepDragging: false

    // Les photos de la tâche, dans l'ordre d'ajout : « avant » puis « après ».
    readonly property var photoShas: task && task.image && task.image.length > 0
                                     ? task.image.split(" ").filter(s => s.length > 0)
                                     : []

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
            text: "Prendre une photo"
            onTriggered: root.capturePhoto()
        }
        MenuItem {
            text: "Choisir une photo"
            onTriggered: root.pickPhoto()
        }
        MenuSeparator {}
        MenuItem {
            text: "Supprimer la tâche"
            onTriggered: deleteDialog.open()
        }
    }

    // Sélecteur de fichiers / capture caméra, chargés au premier besoin.
    Loader { id: pickers }
    Loader {
        id: cameraCapture
        anchors.fill: parent
        z: 20
        active: false
        visible: status === Loader.Ready
        onLoaded: {
            item.itemId = root.itemId
            item.captured.connect(function(path) {
                AppController.setTaskImage(root.itemId, "file://" + path)
            })
            item.closeRequested.connect(function() {
                cameraCapture.active = false
                cameraCapture.source = ""
            })
        }
    }

    function pickPhoto() {
        if (!pickers.item && pickers.status !== Loader.Error)
            pickers.source = "FilePickers.qml"
        if (pickers.item)
            pickers.item.openPhoto(root.itemId)
        else
            AppController.toast("Sélecteur de fichiers indisponible")
    }

    function capturePhoto() {
        // PhotoCapture.qml n'est embarqué que si COLO_HAS_CAMERA (comme ScanPage).
        cameraCapture.source = "PhotoCapture.qml"
        cameraCapture.active = true
        if (cameraCapture.status === Loader.Error)
            AppController.toast("Caméra indisponible sur cet appareil")
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
        // Figée pendant le glissement d'une étape : sinon elle disputerait le
        // geste vertical à la poignée.
        interactive: !root.stepDragging
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

            // --- Échéance : facultative, posée en un tap, envoyable à l'agenda ---
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    Layout.fillWidth: true
                    flat: true
                    implicitHeight: 40
                    contentItem: Label {
                        text: {
                            if (!root.task) return "＋  Ajouter une échéance"
                            if (root.task.dueNote && root.task.dueNote.length > 0)
                                return "Échéance : " + root.task.dueNote
                            if (root.task.due > 0)
                                return "Échéance : " + root.dueText(root.task.due)
                            return "＋  Ajouter une échéance"
                        }
                        color: root.task && root.isOverdue(root.task.due) && !root.task.done
                               && !(root.task.dueNote && root.task.dueNote.length > 0)
                               ? Theme.danger : Theme.accent
                        font.pixelSize: 13
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        wrapMode: Text.WordWrap
                    }
                    background: Rectangle {
                        radius: 10
                        color: parent.pressed ? Theme.surfaceHigh : "transparent"
                        border.color: Theme.outline
                        border.width: 1
                    }
                    onClicked: {
                        dueDialog.targetStep = ""
                        dueDialog.openFor(root.task && root.task.due ? root.task.due : 0,
                                          root.task && root.task.dueNote ? root.task.dueNote : "")
                    }
                }

                // Vers l'agenda du téléphone (ou un .ics sur ordinateur) — dates seules.
                Button {
                    visible: root.task && root.task.due > 0
                             && !(root.task.dueNote && root.task.dueNote.length > 0)
                    flat: true
                    implicitHeight: 40
                    contentItem: Label {
                        text: "Agenda"
                        color: Theme.accent
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 10
                        color: parent.pressed ? Theme.surfaceHigh : "transparent"
                        border.color: Theme.outline
                        border.width: 1
                    }
                    onClicked: AppController.addToCalendar(root.task.name,
                                                          root.task.note || "",
                                                          root.task.due)
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
                text: "Découpez la tâche en étapes — et chaque étape en sous-étapes."
                color: Theme.textDim
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }

            // Les étapes (et sous-étapes) gardent leur place une fois cochées.
            // Indentation = profondeur dans l'arbre. Tap → menu ; poignée → réordonner
            // entre frères (même parent).
            Repeater {
                model: AppController.steps

                delegate: Item {
                    id: stepWrapper
                    Layout.fillWidth: true
                    Layout.leftMargin: model.depth * 18
                    implicitHeight: Math.max(52, stepRowContent.implicitHeight + 12)

                    property int rowIndex: index
                    property bool held: false
                    z: held ? 2 : 1

                    DropArea {
                        anchors.fill: parent
                        onEntered: function (drag) {
                            const source = drag.source
                            if (!source || source === stepWrapper)
                                return
                            AppController.steps.moveItem(source.rowIndex,
                                                         stepWrapper.rowIndex)
                        }
                    }

                    Rectangle {
                        id: stepRow
                        width: stepWrapper.width
                        height: stepWrapper.height
                        radius: 12
                        color: Theme.surface
                        border.color: Theme.outline
                        border.width: 1

                        Drag.active: stepWrapper.held
                        Drag.source: stepWrapper
                        Drag.hotSpot.x: width / 2
                        Drag.hotSpot.y: height / 2
                        opacity: stepWrapper.held ? 0.85 : 1.0
                        states: State {
                            when: stepWrapper.held
                            ParentChange { target: stepRow; parent: root }
                        }

                        RowLayout {
                            id: stepRowContent
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

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 4
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 1

                                Label {
                                    Layout.fillWidth: true
                                    text: model.name
                                    color: model.done ? Theme.textDim : Theme.text
                                    font.pixelSize: 15
                                    font.strikeout: model.done
                                    wrapMode: Text.WordWrap
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: {
                                        const parts = []
                                        if (model.stepsTotal > 0)
                                            parts.push(model.stepsDone + "/" + model.stepsTotal
                                                       + " sous-étape"
                                                       + (model.stepsTotal > 1 ? "s" : ""))
                                        if (model.dueNote && model.dueNote.length > 0)
                                            parts.push(model.dueNote)
                                        else if (model.due > 0)
                                            parts.push(root.dueText(model.due))
                                        return parts.join(" · ")
                                    }
                                    color: root.isOverdue(model.due) && !model.done
                                           && !(model.dueNote && model.dueNote.length > 0)
                                           ? Theme.danger
                                           : (model.stepsTotal > 0
                                              && model.stepsDone === model.stepsTotal
                                              ? Theme.accent : Theme.textDim)
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                }

                                TapHandler {
                                    onTapped: {
                                        stepMenu.stepId      = model.itemId
                                        stepMenu.stepName    = model.name
                                        stepMenu.stepDue     = model.due
                                        stepMenu.stepDueNote = model.dueNote || ""
                                        stepMenu.stepQty     = model.qty
                                        stepMenu.stepNote    = model.note
                                        stepMenu.stepAisle   = model.aisle
                                        stepMenu.popup()
                                    }
                                }
                            }

                            MouseArea {
                                Layout.rightMargin: 2
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: 34
                                Layout.preferredHeight: Theme.touchTarget
                                drag.target: stepWrapper.held ? stepRow : undefined
                                drag.axis: Drag.YAxis
                                cursorShape: Qt.SizeVerCursor
                                preventStealing: true
                                onPressed: { stepWrapper.held = true; root.stepDragging = true }
                                onReleased: { stepWrapper.held = false; root.stepDragging = false }
                                onCanceled: { stepWrapper.held = false; root.stepDragging = false }

                                Icon {
                                    anchors.centerIn: parent
                                    name: "grip"
                                    color: Theme.textDim
                                    size: 16
                                }
                            }
                        }
                    }
                }
            }

            // Ajout d'étape racine (sous la tâche ouverte).
            Button {
                Layout.fillWidth: true
                flat: true
                implicitHeight: Theme.touchTarget
                contentItem: Label {
                    text: "＋  Ajouter une étape"
                    color: Theme.accent
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 12
                    color: parent.pressed ? Theme.surfaceHigh : Theme.surface
                    border.color: Theme.outline
                    border.width: 1
                }
                onClicked: addStepDialog.open()
            }

            // --- Photos (plusieurs : l'état avant, le résultat après…) ---
            Repeater {
                model: root.photoShas

                delegate: Rectangle {
                    id: photoCard
                    required property string modelData
                    required property int index

                    Layout.fillWidth: true
                    implicitHeight: photo.status === Image.Ready
                                    ? photo.paintedHeight + 2 : 110
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
                        source: "image://taskimg/" + photoCard.modelData
                                + "?r=" + AppController.imageRevision
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
                        onClicked: {
                            if (photo.status === Image.Ready)
                                photoViewer.openFor(photoCard.modelData)
                        }
                    }

                    // Retirer CETTE photo (les autres restent) : croix discrète.
                    ToolButton {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: 4
                        implicitWidth: 34
                        implicitHeight: 34
                        background: Rectangle {
                            radius: width / 2
                            color: Qt.rgba(0, 0, 0, 0.45)
                        }
                        contentItem: Icon {
                            name: "close"
                            color: "white"
                            size: 13
                        }
                        onClicked: {
                            removePhotoDialog.sha = photoCard.modelData
                            removePhotoDialog.open()
                        }
                    }
                }
            }

            // Toujours proposer une photo de plus : l'avant appelle son après.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    Layout.fillWidth: true
                    flat: true
                    implicitHeight: Theme.touchTarget
                    contentItem: Label {
                        text: "Prendre une photo"
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
                    onClicked: root.capturePhoto()
                }

                Button {
                    Layout.fillWidth: true
                    flat: true
                    implicitHeight: Theme.touchTarget
                    contentItem: Label {
                        text: "Choisir dans la galerie"
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
            }

            Item { Layout.preferredHeight: 8 }
        }
    }

    function addStep(name) {
        const n = (name || "").trim()
        if (n.length === 0)
            return
        AppController.steps.addItem(n, "", "", "")
    }

    ColoDialog {
        id: addStepDialog
        title: "Nouvelle étape"
        acceptText: "Ajouter"
        acceptEnabled: addStepName.text.trim().length > 0

        ColoTextField {
            id: addStepName
            Layout.fillWidth: true
            hint: "Étape"
            onAccepted: if (addStepDialog.acceptEnabled) addStepDialog.accept()
        }

        onOpened: {
            addStepName.text = ""
            addStepName.forceActiveFocus()
        }
        onAccepted: root.addStep(addStepName.text)
    }

    ColoDialog {
        id: addChildDialog
        title: "Sous-étape"
        acceptText: "Ajouter"
        acceptEnabled: addChildName.text.trim().length > 0

        property string parentStepId: ""

        function openFor(parentId, parentName) {
            parentStepId = parentId
            title = parentName && parentName.length > 0
                    ? "Sous-étape de « " + parentName + " »"
                    : "Sous-étape"
            addChildName.text = ""
            open()
            addChildName.forceActiveFocus()
        }

        ColoTextField {
            id: addChildName
            Layout.fillWidth: true
            hint: "Sous-étape"
            onAccepted: if (addChildDialog.acceptEnabled) addChildDialog.accept()
        }

        onAccepted: AppController.steps.addChild(addChildDialog.parentStepId,
                                                 addChildName.text.trim())
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

        property string sha: ""
        function openFor(s) {
            sha = s
            AppController.requestHd(s)
            open()
        }

        background: Rectangle { color: Qt.rgba(0, 0, 0, 0.92) }

        contentItem: Item {
            Image {
                anchors.fill: parent
                anchors.margins: 8
                source: photoViewer.sha.length > 0
                        ? "image://taskimg/" + photoViewer.sha
                          + "?hd=1&r=" + AppController.imageRevision
                        : ""
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

    // Menu d'une étape : la modifier, son échéance, l'agenda, sa suppression.
    ColoMenu {
        id: stepMenu
        property string stepId: ""
        property string stepName: ""
        property real   stepDue: 0
        property string stepDueNote: ""
        // Champs invisibles à l'écran mais répliqués : conservés tels quels au
        // renommage, pour ne pas écraser une modification distante concurrente.
        property string stepQty: ""
        property string stepNote: ""
        property string stepAisle: ""

        MenuItem {
            text: "Modifier l'étape"
            onTriggered: stepEditDialog.openFor(stepMenu.stepId, stepMenu.stepName)
        }
        MenuItem {
            text: "Ajouter une sous-étape"
            onTriggered: addChildDialog.openFor(stepMenu.stepId, stepMenu.stepName)
        }
        MenuItem {
            text: (stepMenu.stepDue > 0 || stepMenu.stepDueNote.length > 0)
                  ? "Modifier l'échéance" : "Ajouter une échéance"
            onTriggered: {
                dueDialog.targetStep = stepMenu.stepId
                dueDialog.openFor(stepMenu.stepDue, stepMenu.stepDueNote)
            }
        }
        MenuItem {
            text: "Ajouter à l'agenda"
            enabled: stepMenu.stepDue > 0 && stepMenu.stepDueNote.length === 0
            onTriggered: AppController.addToCalendar(
                             stepMenu.stepName + " — " + root.pageTitle, "",
                             stepMenu.stepDue)
        }
        MenuSeparator {}
        MenuItem {
            text: "Supprimer l'étape"
            onTriggered: AppController.steps.removeItem(stepMenu.stepId)
        }
    }

    // Un seul dialogue d'échéance pour la tâche ET ses étapes : `targetStep` dit
    // à qui la date/texte choisi s'applique ("" = la tâche).
    DueDialog {
        id: dueDialog
        property string targetStep: ""
        onPicked: function (ms, note) {
            const model = targetStep.length > 0 ? AppController.steps
                                                : AppController.items
            const id = targetStep.length > 0 ? targetStep : root.itemId
            if (note && note.length > 0) {
                model.setDueNote(id, note)
            } else if (ms > 0) {
                model.setDue(id, ms)
            } else {
                // Tout retirer : date et texte.
                model.setDueNote(id, "")
                model.setDue(id, 0)
            }
        }
    }

    // --- Dialogues ---

    // Renommer une étape. Les autres champs (quantité, note, catégorie) sont
    // repassés inchangés : editItem ne touche pas leur version.
    ColoDialog {
        id: stepEditDialog
        title: "Modifier l'étape"
        acceptText: "Enregistrer"
        acceptEnabled: stepEditName.text.trim().length > 0

        property string stepId: ""

        function openFor(id, name) {
            stepId = id
            stepEditName.text = name
            open()
            stepEditName.forceActiveFocus()
            stepEditName.selectAll()
        }

        ColoTextField {
            id: stepEditName
            Layout.fillWidth: true
            hint: "Étape"
            onAccepted: if (stepEditDialog.acceptEnabled) stepEditDialog.accept()
        }

        onAccepted: AppController.steps.editItem(stepEditDialog.stepId,
                                                 stepEditName.text.trim(),
                                                 stepMenu.stepQty,
                                                 stepMenu.stepNote,
                                                 stepMenu.stepAisle)
    }

    ColoDialog {
        id: removePhotoDialog
        title: "Retirer cette photo ?"
        acceptText: "Retirer"
        destructive: true

        property string sha: ""

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: "La photo sera retirée de la tâche, pour tout le monde. "
                  + "Les autres photos ne bougent pas."
        }

        onAccepted: AppController.removeTaskImage(root.itemId, removePhotoDialog.sha)
    }

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
