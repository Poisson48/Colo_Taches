import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import ColoTaches

// Scan du QR affiché par l'autre appareil. La permission caméra peut être refusée :
// l'écran doit alors expliquer et renvoyer vers le collage de lien, pas rester noir.
Item {
    id: root

    signal joined()
    signal closeRequested()

    // Fond noir : entre l'ouverture et la première image de la caméra, l'écran ne
    // doit pas laisser voir la page en dessous.
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Component.onCompleted: Permissions.requestCamera()

    CaptureSession {
        id: session
        camera: Camera {
            id: camera
            active: Permissions.cameraGranted && root.visible
        }
        videoOutput: preview
    }

    VideoOutput {
        id: preview
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectCrop
        visible: Permissions.cameraGranted
    }

    QrScanner {
        id: scanner
        videoSink: preview.videoSink
        active: Permissions.cameraGranted

        onCodeDetected: function(text) {
            if (AppController.joinList(text)) {
                root.joined()
            } else {
                AppController.toast("Ce QR code n'est pas une invitation Colo Tâches")
                // Réarmer : l'utilisateur peut viser un autre code.
                rearm.restart()
            }
        }
    }

    Timer {
        id: rearm
        interval: 1500
        onTriggered: { scanner.active = false; scanner.active = true }
    }

    // Viseur : un cadre, et le reste assombri.
    Item {
        anchors.fill: parent
        visible: Permissions.cameraGranted

        Rectangle {
            id: frame
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height) * 0.7
            height: width
            color: "transparent"
            border.color: Theme.accent
            border.width: 3
            radius: 16
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: frame.bottom
            anchors.topMargin: 24
            width: parent.width - 60
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Visez le QR code affiché sur l'autre téléphone"
            color: "white"
            font.pixelSize: 15
            style: Text.Outline
            styleColor: "#000000"
        }
    }

    // Permission refusée (ou appareil sans caméra).
    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - 64
        spacing: Theme.gap
        visible: !Permissions.cameraGranted

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "Caméra indisponible"
            color: Theme.text
            font.pixelSize: 19
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Sans accès à la caméra, collez le lien d'invitation : le résultat est le même."
            color: Theme.textDim
            font.pixelSize: 14
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            implicitHeight: Theme.touchTarget
            implicitWidth: 200
            text: "Réessayer"
            background: Rectangle {
                radius: 12
                color: parent.pressed ? Theme.accentDim : Theme.surfaceHigh
            }
            contentItem: Label {
                text: parent.text
                color: Theme.text
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: Permissions.requestCamera()
        }
    }

    // Barre supérieure, par-dessus l'aperçu caméra.
    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 56
        color: Qt.rgba(0, 0, 0, 0.55)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            spacing: 4

            ToolButton {
                Layout.preferredWidth: Theme.touchTarget
                Layout.preferredHeight: Theme.touchTarget
                contentItem: Icon {
                    name: "close"
                    color: "white"
                    size: 17
                }
                onClicked: root.closeRequested()
            }

            Label {
                Layout.fillWidth: true
                text: "Scanner le QR code"
                color: "white"
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
        }
    }
}
