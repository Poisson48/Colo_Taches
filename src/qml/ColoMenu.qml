import QtQuick
import QtQuick.Controls

// Menu qui s'élargit à son plus long libellé.
//
// Le Menu par défaut tire sa largeur de son contentItem (une ListView) qui ne mesure
// pas tous ses délégués : les textes longs se retrouvent tronqués (« Tout remettre à
// ach… »). On calcule donc la largeur sur le plus large des items.
Menu {
    implicitWidth: {
        let w = 0
        for (let i = 0; i < count; ++i) {
            const it = itemAt(i)
            if (it)
                w = Math.max(w, it.implicitWidth)
        }
        return w
    }
}
