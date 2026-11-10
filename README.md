# Colo_Taches

**Liste de tâches partagée — privée, gratuite, sans publicité et sans serveur à soi.**

Une app simple pour s'organiser à plusieurs : trucs à faire, corvées, travaux. Créez une liste, partagez-la par QR code, chacun l'a à jour en temps réel. Découpez une tâche en **étapes** cochables, ajoutez une **photo** (le robinet qui fuit, la teinte de peinture). Tout est chiffré de bout en bout et reste sur vos appareils — aucun compte, aucune donnée collectée, aucune publicité, aucun serveur à héberger. Fonctionne hors ligne, gère les modifications simultanées, autant de listes que de contextes. Open source, GPL v3.

[![CI](https://github.com/Poisson48/Colo_Taches/actions/workflows/ci.yml/badge.svg)](https://github.com/Poisson48/Colo_Taches/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/screenshots/lists.png" width="250" alt="Écran des listes, groupées et partagées">
  &nbsp;
  <img src="docs/screenshots/list.png" width="250" alt="Tâches rangées par catégorie, avec progression des étapes">
  &nbsp;
  <img src="docs/screenshots/shopping.png" width="250" alt="Mode Chantier">
</p>

<p align="center">
  <em>Listes groupées et « partagées avec »&nbsp;·&nbsp;
  Tâches par catégorie, étapes et photos&nbsp;·&nbsp;
  Mode Chantier, les mains occupées</em>
</p>

## Essayer maintenant

Téléchargez la dernière version — **[⬇️ Releases](../../releases/latest)** — et lancez-la, rien à installer :

| Plateforme | Fichier | Pour l'ouvrir |
|---|---|---|
| **PC / Linux** (x86-64) | `ColoTaches-*-x86_64.AppImage` | `chmod +x ColoTaches-*.AppImage && ./ColoTaches-*.AppImage` |
| **Android** (arm64) | `colotaches-*-arm64.apk` | Ouvrez l'APK sur le téléphone (autorisez l'installation depuis cette source), ou `adb install -r colotaches-*-arm64.apk` |

L'AppImage embarque Qt : un fichier, aucune dépendance, il se lance tel quel.
L'APK est signé avec la clé de publication du projet — les versions suivantes
s'installent **par-dessus**, et l'app vous les proposera d'elle-même.

Pour tester à plusieurs : ouvrez l'app sur un premier appareil, créez une liste, partagez
le QR code (ou le lien) aux autres — les modifications s'y synchronisent en direct.

## Comment ça marche

### Architecture
- **Copie locale complète** : chaque appareil stocke toutes les tâches et l'historique en SQLite.
- **Sync chiffrée E2E, hybride** : relais Nostr privé pour le stockage durable + transport P2P WebRTC (signaling/TURN) pour la vitesse. Le relais et le serveur P2P sont deux URL configurables ; les anciens relais publics restent en transition. Aucune donnée de liste n'est stockée sur le signaling.
- **Sans conflits** : CRDT maison avec Last-Writer-Wins par champ (nom, catégorie, coché, photo…) et horloges de Lamport. Les modifications simultanées convergent identiquement sur tous les appareils, sans intervention.
- **Appairage simple** : créez une liste, puis soit l'autre appareil **scanne le QR code**, soit vous lui **envoyez le lien** `colotaches://` (WhatsApp, SMS…) — un appui dessus ouvre l'app et rejoint la liste. Le lien porte la clé : ne le publiez pas.
- **Plusieurs listes, groupées** : autant de listes qu'il y a de contextes, rangées en **groupes** nommés (Maison, Boulot…). Chaque liste indique **avec qui elle est partagée**.
- **Hors ligne d'abord** : les modifications s'ajoutent immédiatement localement dans une file d'attente. Au retour du réseau, la sync reprend automatiquement, et l'app **confirme** quand tout est parti. Aucune modification n'est supprimée sans accusé durable du relais.
- **Photos** : vignette légère sur le relais, **haute résolution en P2P** à la demande (≤ 2 Mo).

### Au quotidien
- **Étapes** : découpez une tâche (« Repeindre le volet » → démonter, poncer, repeindre) en étapes cochables. La liste affiche la progression (« 2/5 étapes »), chacun voit où en est le travail. Supprimer la tâche emporte ses étapes.
- **Photos** : plusieurs images par tâche — l'état **avant**, le résultat **après**, la référence à racheter. Compressées automatiquement et synchronisées chiffrées comme le reste ; tap pour afficher en plein écran.
- **Historique** : chaque tâche cochée laisse une trace durable (qui, quand), même une fois retirée de la liste — de quoi voir ce qui a déjà été fait, jour par jour.
- **Échéances** : facultatives, sur les tâches comme sur les étapes — « pour demain », « en retard » en rouge. Un bouton **Agenda** envoie l'échéance dans le calendrier du téléphone (ou un fichier .ics sur ordinateur).
- **Catégories** : classez les tâches (Ménage, Cuisine, Jardin, Bricolage…, ou vos propres catégories). La liste se regroupe en sections. La catégorie d'une nouvelle tâche est **pré-remplie** d'après celles déjà classées (« tondre » → Jardin) — proposée, jamais imposée. Réordonner via glisser-déposer.
- **Favoris appris à l'usage** : les tâches récurrentes (poubelles, arrosage…) réapparaissent en pastilles sous la barre d'ajout, à remettre d'un tap.
- **Mode Chantier** : les mains occupées, toute la ligne devient une case à cocher, une barre suit l'avancement, l'écran reste allumé et cocher fait vibrer.
- **Détails** par tâche, date d'ajout et de réalisation, qui a ajouté quoi.
- **Import / export** : une liste en CSV (étapes comprises), ou toutes vos listes en ZIP ; réimport d'un CSV ou d'un ZIP.
- **Mises à jour dans l'app** (Android) : l'app détecte les nouvelles versions, les télécharge et les installe.

### Cycle de vie
1. Créer ou rejoindre une liste via QR code / URI.
2. Ajouter / cocher / supprimer des tâches et étapes localement (instantané).
3. Chiffrer et envoyer vers les relais Nostr (les photos partent dans des événements dédiés).
4. Réception et merge automatiques chez les autres : convergence garantie.
5. Notifications locales à la réception de changements (quand l'app est ouverte).

## Build & installation

### Prérequis
- **Ubuntu/Debian** : `bash scripts/setup-dev.sh` installe les dépendances (Qt 6, libsodium, libsecp256k1, CMake, Ninja).
- **Autres OS** : adapter manuellement selon les paquets disponibles. Voir `scripts/setup-dev.sh` pour la liste complète.

### Build desktop
```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Binaire exécutable : `build/src/colotaches`

### Android (arm64), build local

~7 Go de téléchargements (SDK, NDK, Qt pour Android) :
```bash
bash scripts/setup-android.sh   # une fois
bash scripts/build-android.sh   # → colotaches-arm64.apk, signé
```

### Publier une version

```bash
git tag -a v1.0.0 -m "…" && git push origin v1.0.0
```
Le workflow *Release* construit l'APK Android **et** l'AppImage Linux, puis les publie
dans les Releases GitHub. Le message du tag devient les notes de version (affichées
dans l'app avant une mise à jour).

## État du projet

Fork de [Colo_Course](https://github.com/Poisson48/Colo_Course) (liste de courses partagée), adapté au partage de tâches, avec deux nouveautés : les **étapes** et les **photos** de tâches. Même architecture éprouvée : tests CRDT complets, sync relais, chiffrement E2E, appairage par QR.

Spécification technique (CRDT, protocole relais, format données) : [docs/SPEC.md](docs/SPEC.md)  
Transport hybride, P2P, TURN et self-host : [docs/SYNC-HYBRID.md](docs/SYNC-HYBRID.md)

## Limitations connues

- **Notifications hors app** : l'app doit être ouverte (ou en premier plan sur mobile) pour recevoir les changements en temps réel. Pas de push notification Firebase ou FCM par choix architectural (pas de serveur tiers, de compte requis).
- **Révocation d'un membre** : non supportée en v1. Pour exclure quelqu'un d'une liste, recréez-la et repartagez la clé avec les nouveaux membres uniquement.
- **Photos** : vignette ~30 Ko durable ; la HD est un bonus P2P, pas une garantie si aucun pair ne la conserve.
- **4G / NAT** : le P2P 4G nécessite TURN ; sans TURN joignable, retour au relais (sans perte).
- **Nettoyage des tombstones** : les éléments supprimés sont purgés localement après 30 jours sans modification. Un pair très en retard peut théoriquement ressusciter un item purgé (très rare, accepté).

## Licence

GPLv3. Voir [LICENSE](LICENSE).
Basé sur Qt 6 (LGPL), libsodium (ISC), libsecp256k1 (MIT).
