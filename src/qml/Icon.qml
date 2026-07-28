import QtQuick

// Icônes dessinées, pas écrites.
//
// Les glyphes « ⋮ », « ⠿ », « ✓ », « ← » ne sont pas garantis dans les polices d'Android :
// quand ils manquent, le système affiche un carré (le fameux « tofu »), et le bouton
// devient illisible. Un tracé, lui, ne peut pas manquer.
Item {
    id: icon

    // "menu" | "grip" | "close" | "back" | "check" | "search"
    property string name: "menu"
    property color  color: "#FFFFFF"

    // Taille du tracé. Elle est distincte de celle de l'Item : un ToolButton étire son
    // contentItem sur toute sa surface (48×48), et l'icône se dessinerait en grand.
    // On dessine donc dans un carré de `size`, centré, quelle que soit la place reçue.
    property real size: 20
    property real thickness: Math.max(1.5, size / 11)

    implicitWidth: size
    implicitHeight: size

    onColorChanged: strokes.requestPaint()
    onNameChanged: strokes.requestPaint()
    onSizeChanged: strokes.requestPaint()

    // --- Icônes en points : trois points verticaux (menu), grille 2×3 (poignée) ---
    Grid {
        anchors.centerIn: parent
        visible: icon.name === "menu" || icon.name === "grip"

        readonly property real dot: Math.max(2, icon.size / 7)

        columns: icon.name === "grip" ? 2 : 1
        rows: 3
        spacing: icon.size / 7

        Repeater {
            model: icon.name === "grip" ? 6 : 3
            Rectangle {
                width: parent.dot
                height: parent.dot
                radius: parent.dot / 2
                color: icon.color
            }
        }
    }

    // --- Icônes au trait ---
    Canvas {
        id: strokes
        anchors.centerIn: parent
        width: icon.size
        height: icon.size
        visible: !(icon.name === "menu" || icon.name === "grip")

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = icon.color
            ctx.lineWidth = icon.thickness
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            const w = width
            const h = height
            ctx.beginPath()

            if (icon.name === "close") {
                const m = w * 0.28
                ctx.moveTo(m, m);         ctx.lineTo(w - m, h - m)
                ctx.moveTo(w - m, m);     ctx.lineTo(m, h - m)

            } else if (icon.name === "back") {
                // Chevron vers la gauche, prolongé d'une barre : une flèche de retour.
                const x = w * 0.34
                ctx.moveTo(x + w * 0.16, h * 0.24)
                ctx.lineTo(x - w * 0.06, h * 0.5)
                ctx.lineTo(x + w * 0.16, h * 0.76)
                ctx.moveTo(x - w * 0.04, h * 0.5)
                ctx.lineTo(w * 0.80, h * 0.5)

            } else if (icon.name === "check") {
                ctx.moveTo(w * 0.22, h * 0.52)
                ctx.lineTo(w * 0.42, h * 0.72)
                ctx.lineTo(w * 0.78, h * 0.28)

            } else if (icon.name === "search") {
                const r = w * 0.26
                const cx = w * 0.44
                const cy = h * 0.44
                ctx.arc(cx, cy, r, 0, Math.PI * 2)
                ctx.moveTo(cx + r * 0.72, cy + r * 0.72)
                ctx.lineTo(w * 0.82, h * 0.82)
            }

            ctx.stroke()
        }
    }
}
