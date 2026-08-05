import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Échéance : une date (tambours) OU un texte libre (« avant que ça ne pue »).
// Les deux s'excluent : poser l'un retire l'autre. `picked(ms, note)` — ms=0 et
// note vide = sans échéance.
ColoDialog {
    id: dlg

    title: "Échéance"
    acceptText: "Définir"
    acceptEnabled: mode === "date" || textField.text.trim().length > 0
                   || (currentMs > 0 || currentNote.length > 0)

    property real   currentMs: 0
    property string currentNote: ""
    // "date" | "text"
    property string mode: "date"

    signal picked(real ms, string note)

    readonly property var monthNames: ["janv.", "févr.", "mars", "avril", "mai", "juin",
                                       "juil.", "août", "sept.", "oct.", "nov.", "déc."]
    readonly property int baseYear: new Date().getFullYear()

    function openFor(ms, note) {
        currentMs = ms || 0
        currentNote = note || ""
        mode = (currentNote && currentNote.length > 0) ? "text" : "date"
        textField.text = currentNote
        const d = currentMs > 0 ? new Date(currentMs) : new Date()
        dayTumbler.currentIndex   = d.getDate() - 1
        monthTumbler.currentIndex = d.getMonth()
        yearTumbler.currentIndex  = Math.max(0, Math.min(3, d.getFullYear() - baseYear))
        open()
        if (mode === "text")
            textField.forceActiveFocus()
    }

    function setFromDate(d) {
        dayTumbler.currentIndex   = d.getDate() - 1
        monthTumbler.currentIndex = d.getMonth()
        yearTumbler.currentIndex  = Math.max(0, Math.min(3, d.getFullYear() - baseYear))
    }

    // Basculer date ↔ texte.
    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 36
            contentItem: Label {
                text: "Date"
                color: dlg.mode === "date" ? Theme.onAccent : Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 10
                color: dlg.mode === "date" ? Theme.accent
                     : (parent.pressed ? Theme.surfaceHigh : Theme.surfaceHigh)
                border.color: Theme.outline
                border.width: dlg.mode === "date" ? 0 : 1
            }
            onClicked: dlg.mode = "date"
        }

        Button {
            Layout.fillWidth: true
            flat: true
            implicitHeight: 36
            contentItem: Label {
                text: "Texte libre"
                color: dlg.mode === "text" ? Theme.onAccent : Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 10
                color: dlg.mode === "text" ? Theme.accent
                     : (parent.pressed ? Theme.surfaceHigh : Theme.surfaceHigh)
                border.color: Theme.outline
                border.width: dlg.mode === "text" ? 0 : 1
            }
            onClicked: {
                dlg.mode = "text"
                textField.forceActiveFocus()
            }
        }
    }

    // --- Mode date ---
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.gap
        visible: dlg.mode === "date"

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
    }

    // --- Mode texte ---
    ColoTextField {
        id: textField
        Layout.fillWidth: true
        visible: dlg.mode === "text"
        hint: "ex. avant que ça ne pue"
        onAccepted: if (dlg.acceptEnabled) dlg.accept()
    }

    Button {
        Layout.fillWidth: true
        visible: dlg.currentMs > 0 || dlg.currentNote.length > 0
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
            dlg.picked(0, "")
        }
    }

    onAccepted: {
        if (mode === "text") {
            const t = textField.text.trim()
            if (t.length > 0)
                picked(0, t)
            else
                picked(0, "")
            return
        }
        const year  = baseYear + yearTumbler.currentIndex
        const month = monthTumbler.currentIndex
        const maxDay = new Date(year, month + 1, 0).getDate()
        const day   = Math.min(dayTumbler.currentIndex + 1, maxDay)
        picked(new Date(year, month, day).getTime(), "")
    }
}
