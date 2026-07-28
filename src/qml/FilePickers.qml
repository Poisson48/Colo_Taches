import QtQuick
import QtQuick.Dialogs

// Sélecteurs de fichiers natifs pour l'export/import. Isolés ici parce qu'ils importent
// QtQuick.Dialogs, absent de certains kits Qt de bureau : l'écran des listes se charge
// sans, et ce fichier n'est instancié qu'au moment où l'on exporte ou importe.
// Sur Android, ces dialogues sont l'explorateur du système (SAF) et renvoient une URI
// content:// que le C++ sait lire et écrire.
Item {
    id: pickers

    function openExportCsv(listId) {
        exportCsvDialog.listId = listId
        exportCsvDialog.selectedFile = "file:///" + AppController.suggestedFileName(listId)
        exportCsvDialog.open()
    }
    function openExportZip() {
        exportZipDialog.selectedFile = "file:///mes-listes.zip"
        exportZipDialog.open()
    }
    function openImport() {
        importDialog.open()
    }
    // Photo d'une tâche : sélecteur d'images du système (galerie sur Android).
    function openPhoto(itemId) {
        photoDialog.itemId = itemId
        photoDialog.open()
    }

    FileDialog {
        id: exportCsvDialog
        fileMode: FileDialog.SaveFile
        nameFilters: ["Fichier CSV (*.csv)"]
        defaultSuffix: "csv"
        property string listId: ""
        onAccepted: AppController.exportListCsv(selectedFile, listId)
    }

    FileDialog {
        id: exportZipDialog
        fileMode: FileDialog.SaveFile
        nameFilters: ["Archive ZIP (*.zip)"]
        defaultSuffix: "zip"
        onAccepted: AppController.exportAllZip(selectedFile)
    }

    // Un .csv (une liste) ou un .zip (plusieurs). L'import crée de nouvelles listes,
    // il ne touche jamais aux existantes.
    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        nameFilters: ["Listes (*.csv *.zip)", "Tous les fichiers (*)"]
        onAccepted: AppController.importFile(selectedFile)
    }

    // La photo est réduite et rattachée à la tâche par le C++ (setTaskImage).
    FileDialog {
        id: photoDialog
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.jpg *.jpeg *.png *.webp *.bmp)", "Tous les fichiers (*)"]
        property string itemId: ""
        onAccepted: AppController.setTaskImage(itemId, selectedFile)
    }
}
