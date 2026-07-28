import QtQuick
import QtQuick.Controls

// Champ de saisie sombre : fond plein plutôt que soulignement, pour rester lisible
// et offrir une cible tactile pleine hauteur.
TextField {
    id: field

    // Le style Material de Qt 6.8 fait flotter le placeholder au-dessus du champ,
    // où notre bordure le traverse et le barre. On l'efface dès qu'il ne sert plus.
    property string hint: ""
    placeholderText: (activeFocus || length > 0) ? "" : hint

    implicitHeight: 52
    leftPadding: 14
    rightPadding: 14
    color: Theme.text
    placeholderTextColor: Theme.textDim
    font.pixelSize: 16
    selectByMouse: true

    background: Rectangle {
        radius: 12
        color: Theme.surfaceHigh
        border.width: field.activeFocus ? 2 : 1
        border.color: field.activeFocus ? Theme.accent : Theme.outline
    }
}
