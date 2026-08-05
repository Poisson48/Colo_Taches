#pragma once

#include <cstdint>
#include <string>
#include <compare>
#include <vector>

namespace core {

// §1 — Logical timestamp: (lamport, deviceId) with total order.
struct Ver {
    int64_t lamport = 0;
    std::string deviceId;

    // Total order: lamport first, then lexicographic deviceId.
    auto operator<=>(const Ver& o) const noexcept {
        if (lamport != o.lamport)
            return lamport <=> o.lamport;
        return deviceId <=> o.deviceId;
    }
    bool operator==(const Ver& o) const noexcept = default;
};

// §2.1 — Tâche (ou étape d'une tâche). Each user-visible field carries its own version.
struct Item {
    // Immutable identity (set at creation, never versioned).
    std::string listId;
    std::string itemId;
    int64_t     created = 0; // ms epoch
    std::string by;          // deviceId of creator

    // Étape : itemId de la tâche parente, "" = tâche de premier niveau. Immuable
    // (fixé à la création, comme `created`) : une étape ne change jamais de tâche.
    std::string parentId;

    // Versioned fields (LWW).
    std::string name;
    Ver         nameVer;

    std::string qty;
    Ver         qtyVer;

    // Précision libre : « 6 couches épaisses », « la marque bleue », « sans sucre ».
    std::string note;
    Ver         noteVer;

    // Rayon du magasin (« Crèmerie »), vide si non classé. Le libellé lui-même sert de
    // clé : un pair qui ne connaît pas un rayon l'affiche quand même, sans rien perdre.
    std::string aisle;
    Ver         aisleVer;

    // Position manuelle dans la liste. Valeurs espacées (initialisées sur `created`,
    // en millisecondes) : réordonner = se glisser au milieu de l'intervalle voisin,
    // sans avoir à renuméroter les autres — donc sans les faire entrer en conflit.
    int64_t order = 0;
    Ver     orderVer;

    // Photos de la tâche : shas256 hex des blobs JPEG, séparés par des espaces
    // ("" = aucune). Champ LWW comme les autres ; les blobs circulent dans des
    // événements « img » séparés et vivent dans la table images.
    std::string image;
    Ver         imageVer;

    // Échéance (ms epoch au minuit local du jour visé, 0 = pas d'échéance).
    // Facultative, valable pour une tâche comme pour une étape.
    int64_t due = 0;
    Ver     dueVer;

    // Échéance libre (« avant que ça ne pue ») : alternative à `due`. Date et texte
    // s'excluent côté écriture locale (poser l'un vide l'autre) ; au merge chacun
    // reste LWW indépendant — l'UI privilégie l'affichage de dueNote s'il est non vide.
    std::string dueNote;
    Ver         dueNoteVer;

    bool done = false;
    Ver  doneVer;
    // Date de cochage (ms epoch, 0 si à acheter). Satellite de `done` : pas de version
    // propre, il suit doneVer — celui qui gagne le merge sur `done` apporte sa date.
    int64_t doneAt = 0;

    bool del = false;
    Ver  delVer;

    // Local-only metadata (§2.4 / §6): ms epoch of last local write. Never synchronised.
    int64_t touched = 0;
};

// §2.2 — List metadata.
struct ListMeta {
    std::string listId;
    std::vector<uint8_t> key; // 32 bytes

    std::string title;
    Ver         titleVer;

    // Mode de classement des articles, répliqué (LWW) comme le titre : partagé par tous
    // les participants. "" ou "aisle" = rangé par rayon puis position manuelle (défaut),
    // "manual" = position manuelle seule (pas de sections, glisser réordonne librement).
    std::string sortMode;
    Ver         sortModeVer;

    int64_t lamport  = 0;  // current Lamport clock for this list
    int64_t lastSync = 0;  // ms epoch of last relay sync
    int64_t created  = 0;  // ms epoch of list creation

    // Groupe local (« Maison », « Boulot »…), vide = non rangé. Purement local et non
    // synchronisé : chaque appareil organise ses listes comme il veut, la même liste
    // partagée peut être dans « Maison » chez l'un et « Courses » chez l'autre.
    std::string groupId;

    // Position manuelle de la liste dans son groupe (local, non synchronisé). Comme
    // pour les articles : valeurs espacées, initialisées sur `created`, pour se glisser
    // entre deux voisines sans renuméroter. 0 = non défini → traité comme `created`.
    int64_t listOrder = 0;
};

} // namespace core
