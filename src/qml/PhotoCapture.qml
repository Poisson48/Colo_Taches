import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import ColoTaches

// Capture d'une photo pour une tâche. Isolé (comme ScanPage) pour n'embarquer
// QtMultimedia que si COLO_HAS_CAMERA. Émet `captured(path)` puis se ferme.
Item {
    id: root

    property string itemId: ""

    signal captured(string path)
    signal closeRequested()

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
        imageCapture: ImageCapture {
            id: imageCapture
            onImageSaved: function(requestId, path) {
                root.captured(path)
                root.closeRequested()
            }
            onErrorOccurred: function(requestId, error, message) {
                AppController.toast("Capture impossible : " + message)
            }
        }
        videoOutput: preview
    }

    VideoOutput {
        id: preview
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectCrop
        visible: Permissions.cameraGranted
    }

    Label {
        anchors.centerIn: parent
        visible: !Permissions.cameraGranted
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: "white"
        font.pixelSize: 15
        text: "Autorisez l'accès à la caméra pour photographier la tâche."
    }

    Button {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        visible: !Permissions.cameraGranted
        text: "Autoriser la caméra"
        onClicked: Permissions.requestCamera()
    }

    // Déclencheur de capture.
    RoundButton {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 36
        visible: Permissions.cameraGranted
        width: 72
        height: 72
        background: Rectangle {
            radius: width / 2
            color: parent.pressed ? Theme.accentDim : Theme.accent
            border.color: "white"
            border.width: 3
        }
        onClicked: {
            // Fichier temporaire : setTaskImage lit le chemin puis compresse.
            const path = AppController.tempPhotoPath()
            if (!path || path.length === 0) {
                AppController.toast("Impossible de préparer le fichier photo")
                return
            }
            imageCapture.captureToFile(path)
        }
    }

    ToolButton {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        width: Theme.touchTarget
        height: Theme.touchTarget
        contentItem: Icon {
            name: "close"
            color: "white"
            size: 22
        }
        onClicked: root.closeRequested()
    }
}
