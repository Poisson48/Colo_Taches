import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Choix d'une échéance : raccourcis pour l'ordinaire (aujourd'hui, demain, dans une
// semaine), tambours jour/mois/année pour le reste. `picked(0)` = sans échéance.
ColoDialog {
    id: dlg

    title: "Échéance"
    acceptText: "Définir"

    // Échéance actuelle (ms epoch, 0 = aucune) : préposition les tambours à l'ouverture.
    property real currentMs: 0

    signal picked(real ms)

    readonly property var monthNames: ["janv.", "févr.", "mars", "avril", "mai", "juin",
                                       "juil.", "août", "sept.", "oct.", "nov.", "déc."]
    readonly property int baseYear: new Date().getFullYear()

    function openFor(ms) {
        currentMs = ms
        const d = ms > 0 ? new Date(ms) : new Date()
        dayTumbler.currentIndex   = d.getDate() - 1
        monthTumbler.currentIndex = d.getMonth()
        yearTumbler.currentIndex  = Math.max(0, Math.min(3, d.getFullYear() - baseYear))
        open()
    }

    function setFromDate(d) {
        dayTumbler.currentIndex   = d.getDate() - 1
        monthTumbler.currentIndex = d.getMonth()
        yearTumbler.currentIndex  = Math.max(0, Math.min(3, d.getFullYear() - baseYear))
    }

    // Raccourcis : l'essentiel des échéances domestiques se pose en un tap.
    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Repeater {
            model: [
                { label: "Aujourd'hui", days: 0 },
                { label: "Demain",      days: 1 },
                { label: "Dans 1 sem.", days: 7 },
            ]

            delegate: Button {
                required property var modelData
                Layout.fillWidth: true
                flat: true
                implicitHeight: 36
                contentItem: Label {
                    text: parent.modelData.label
                    color: Theme.accent
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 10
                    color: parent.pressed ? Theme.accentSoft : Theme.surfaceHigh
                    border.color: Theme.outline
                    border.width: 1
                }
                onClicked: {
                    const d = new Date()
                    d.setDate(d.getDate() + modelData.days)
                    dlg.setFromDate(d)
                }
            }
        }
    }

    // Tambours jour / mois / année, pour les dates qui ne sont pas « bientôt ».
    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 120
        spacing: 0

        Tumbler {
            id: dayTumbler
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: 31
            visibleItemCount: 3
            delegate: Label {
                text: modelData + 1
                color: Tumbler.displacement === 0 ? Theme.text : Theme.textDim
                font.pixelSize: Tumbler.displacement === 0 ? 18 : 14
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                opacity: 1.0 - Math.abs(Tumbler.displacement) / 2
            }
        }

        Tumbler {
            id: monthTumbler
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: dlg.monthNames
            visibleItemCount: 3
            delegate: Label {
                text: modelData
                color: Tumbler.displacement === 0 ? Theme.text : Theme.textDim
                font.pixelSize: Tumbler.displacement === 0 ? 18 : 14
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                opacity: 1.0 - Math.abs(Tumbler.displacement) / 2
            }
        }

        Tumbler {
            id: yearTumbler
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: [dlg.baseYear, dlg.baseYear + 1, dlg.baseYear + 2, dlg.baseYear + 3]
            visibleItemCount: 3
            delegate: Label {
                text: modelData
                color: Tumbler.displacement === 0 ? Theme.text : Theme.textDim
                font.pixelSize: Tumbler.displacement === 0 ? 18 : 14
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                opacity: 1.0 - Math.abs(Tumbler.displacement) / 2
            }
        }
    }

    // Retirer une échéance existante, sans passer par les tambours.
    Button {
        Layout.fillWidth: true
        visible: dlg.currentMs > 0
        flat: true
        implicitHeight: Theme.touchTarget
        contentItem: Label {
            text: "Retirer l'échéance"
            color: Theme.danger
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
        onClicked: {
            dlg.close()
            dlg.picked(0)
        }
    }

    onAccepted: {
        const year  = baseYear + yearTumbler.currentIndex
        const month = monthTumbler.currentIndex
        // 31 février n'existe pas : borner au dernier jour du mois choisi.
        const maxDay = new Date(year, month + 1, 0).getDate()
        const day   = Math.min(dayTumbler.currentIndex + 1, maxDay)
        // Minuit local du jour visé : une échéance est un jour, pas une heure.
        picked(new Date(year, month, day).getTime())
    }
}
