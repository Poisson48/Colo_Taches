import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Historique d'une liste : tout ce qui a déjà été fait, par qui et quand — y compris
// les tâches retirées de la liste depuis. Journal local à cet appareil (chaque
// participant garde sa propre trace), groupé par jour, le plus récent en haut.
Item {
    id: root

    required property string listId

    readonly property string pageTitle: "Historique"

    property var entries: []

    // Le jour est ajouté à chaque entrée : c'est lui qui fait les sections de la vue
    // (le modèle doit porter la clé de section, un délégué ne suffit pas).
    function loadEntries() {
        const list = AppController.history(listId)
        for (let i = 0; i < list.length; ++i)
            list[i].day = dayLabel(list[i].doneAt)
        entries = list
    }

    Component.onCompleted: loadEntries()

    // Une tâche vient d'être cochée (ici ou chez un autre) : l'historique suit.
    Connections {
        target: AppController.items
        function onRefreshed() {
            root.loadEntries()
        }
    }

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
            onClicked: historyMenu.popup()
        }
    }

    ColoMenu {
        id: historyMenu

        MenuItem {
            text: "Vider l'historique"
            enabled: root.entries.length > 0
            onTriggered: clearDialog.open()
        }
    }

    // Jour d'une entrée, pour les en-têtes de section.
    function dayLabel(ms) {
        const d = new Date(ms)
        const now = new Date()
        if (d.toDateString() === now.toDateString())
            return "Aujourd'hui"
        if (new Date(now.getTime() - 86400000).toDateString() === d.toDateString())
            return "Hier"
        return Qt.formatDate(d, "dddd d MMMM")
    }

    ListView {
        anchors.fill: parent
        clip: true
        topMargin: Theme.gap
        bottomMargin: Theme.gap
        spacing: 6
        model: root.entries

        section.property: "day"
        section.criteria: ViewSection.FullString
        section.delegate: Item {
            required property string section
            width: ListView.view.width
            height: 34

            Label {
                anchors.left: parent.left
                anchors.leftMargin: Theme.gap + 4
                anchors.verticalCenter: parent.verticalCenter
                text: parent.section
                color: Theme.accent
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
            }
        }

        delegate: Rectangle {
            id: row
            required property var modelData

            width: ListView.view.width - 2 * Theme.gap
            x: Theme.gap
            height: 56
            radius: 12
            color: Theme.surface
            border.color: Theme.outline
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Icon {
                    Layout.alignment: Qt.AlignVCenter
                    name: "check"
                    color: Theme.accent
                    size: 16
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 1

                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        color: Theme.text
                        font.pixelSize: 15
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: {
                            const parts = []
                            if (row.modelData.byName && row.modelData.byName.length > 0)
                                parts.push("par " + row.modelData.byName)
                            parts.push("à " + Qt.formatDateTime(
                                           new Date(row.modelData.doneAt), "HH:mm"))
                            if (row.modelData.aisle && row.modelData.aisle.length > 0)
                                parts.push(row.modelData.aisle)
                            return parts.join(" · ")
                        }
                        color: Theme.textDim
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // État vide : dire à quoi sert la page plutôt que de montrer du blanc.
    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 80
        visible: root.entries.length === 0
        spacing: 6

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "Rien encore"
            color: Theme.text
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Chaque tâche cochée laisse une trace ici — même une fois "
                  + "retirée de la liste."
            color: Theme.textDim
            font.pixelSize: 14
        }
    }

    ColoDialog {
        id: clearDialog
        title: "Vider l'historique ?"
        acceptText: "Vider"
        destructive: true

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 14
            text: "L'historique de cet appareil sera effacé. Celui des autres "
                  + "participants ne bouge pas, et la liste elle-même non plus."
        }

        onAccepted: {
            AppController.clearHistory(root.listId)
            root.entries = []
        }
    }
}
