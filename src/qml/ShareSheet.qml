import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Feuille de partage : QR à scanner en face-à-face, ou lien à envoyer à distance.
// Le lien contient la clé de chiffrement — d'où l'avertissement.
Popup {
    id: sheet

    property string listId: ""
    property string listName: ""
    property string uri: ""

    function openFor(id, name) {
        listId = id
        listName = name
        uri = AppController.joinUri(id)
        open()
    }

    parent: Overlay.overlay
    width: parent.width
    y: parent.height - height
    modal: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.6) }

    background: Rectangle {
        color: Theme.surface
        radius: 24
        border.color: Theme.outline
        border.width: 1
    }

    ColumnLayout {
        width: parent.width
        spacing: Theme.gap

        // Poignée : signale que la feuille se referme en tirant/tapant à côté.
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 10
            width: 40; height: 4; radius: 2
            color: Theme.outline
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            horizontalAlignment: Text.AlignHCenter
            text: "Partager « " + sheet.listName + " »"
            color: Theme.text
            font.pixelSize: 18
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        // Cadre blanc : un QR sur fond sombre n'est pas lisible par les scanners.
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 232; height: 232
            radius: 12
            color: "white"

            Image {
                anchors.centerIn: parent
                width: 208; height: 208
                smooth: false
                fillMode: Image.PreserveAspectFit
                source: sheet.uri.length > 0 ? "image://qr/" + sheet.uri : ""
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Ce lien contient la clé de la liste : ne le publiez pas."
            color: Theme.textDim
            font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            Layout.bottomMargin: 24
            spacing: Theme.gap

            Button {
                Layout.fillWidth: true
                implicitHeight: 52
                text: "Copier"
                background: Rectangle {
                    radius: 12
                    color: parent.pressed ? Theme.outline : Theme.surfaceHigh
                }
                contentItem: Label {
                    text: parent.text
                    color: Theme.text
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: { AppController.copyToClipboard(sheet.uri); sheet.close() }
            }

            Button {
                Layout.fillWidth: true
                implicitHeight: 52
                text: "Envoyer le lien"
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
                onClicked: { AppController.shareText(sheet.uri); sheet.close() }
            }
        }
    }
}
