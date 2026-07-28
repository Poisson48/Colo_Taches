# Colo_Taches — Plan d'action et workflow

> **Ce fichier est la référence unique.** Toute session (quel que soit le modèle) commence par
> lire ce fichier + `docs/SPEC.md`. Ne pas relire tout le dépôt.
> Ne pas re-discuter les décisions ci-dessous : elles sont actées.
>
> **Note fork** : Colo_Taches est le fork « partage de tâches » de Colo_Course. Les
> phases ci-dessous décrivent la construction de l'app d'origine et restent la
> référence d'architecture. Les ajouts propres à Colo_Taches : items enfants
> (étapes, champ immuable `parent`), photo par tâche (champ LWW `image` + événement
> `img` transportant le blob), vocabulaire tâches/catégories, Mode Chantier.

## 1. Le produit

App de liste de tâches partagée (couples, colocataires) : trucs à faire, corvées, travaux.
Sans abonnement, sans serveur à payer. Appairage par QR code / clé. Chacun garde une copie
locale complète. Ajout / retrait / cochage synchronisés à chaque modif, gestion des modifs
simultanées, plusieurs listes avec des participants différents. Étapes cochables et photo
par tâche. Notification quand des changements arrivent hors ligne. Open source.

## 2. Architecture (décisions actées)

- **Qt 6 / QML** (UI) + **C++** (logique). Cible : Android d'abord, desktop Linux pour débugger.
- **CRDT fait maison** : set d'items avec tombstones, Last-Writer-Wins par champ
  (nom, quantité, coché), horloges de Lamport + ID d'appareil. Gère N participants.
- **SQLite** pour la persistance locale.
- **Transport : relais Nostr publics** (store-and-forward, gratuit, asynchrone) via `QWebSocket`.
  Pas de P2P WebRTC : les deux appareils ne sont jamais en ligne en même temps, et le CGNAT
  mobile fait échouer STUN trop souvent. Bonus : sync LAN directe (mDNS) sur le même Wi-Fi.
- **Chiffrement E2E** : une clé symétrique par liste, échangée dans le QR code d'appairage.
  Les relais ne voient que des blobs chiffrés. Une liste = un canal = une clé.
- **Licence : GPLv3** (compatible Qt open source).

**Dépendances** (règle §4.5 : toute nouvelle dépendance est listée ici) : Qt 6 (Core, Quick,
QuickControls2, Sql, WebSockets), libsodium, libsecp256k1, qrcodegen (vendored, encode le QR),
**Qt Multimedia** (caméra) et **zxing-cpp v2.2.1** (décode le QR ; FetchContent, lecteurs seuls).
Les deux dernières sont **optionnelles** : sans Qt Multimedia, l'app build sans scanner et
l'appairage se fait par lien.

## 3. Workflow git à 3 rôles (économie de tokens)

Chaque tâche du plan passe par 3 sessions distinctes, chacune avec le modèle le moins cher
capable de faire le travail :

| Rôle | Modèle | Effort / réflexion | Ce qu'il fait | Ce qu'il ne fait PAS |
|---|---|---|---|---|
| **Codeur** | Sonnet (Haiku si la tâche est marquée boilerplate) | standard ; réflexion étendue seulement si bloqué | Lit PLAN.md + SPEC.md + les seuls fichiers concernés. Code sur une branche `feat/<tâche>`. Écrit les tests avec le code. Commit(s) atomiques. | Ne merge pas. Ne retouche pas la spec. Ne relit pas tout le dépôt. |
| **Validateur** | Haiku | minimal | Build + lance toute la suite de tests. Relit uniquement `git diff main...feat/<tâche>`. Vérifie la checklist §4. Verdict : OK ou liste de défauts renvoyée au codeur. | Ne corrige pas lui-même (sauf typo triviale). Ne relit pas le code inchangé. |
| **Mergeur** | Haiku | minimal | Si verdict OK : `git merge --squash` dans `main`, message en conventional commits, tag de phase le cas échéant. | Ne merge jamais sans verdict OK. En cas de conflit : escalade à Sonnet. |

**Escalade** (seule exception au tableau) : bug de merge CRDT, corruption de données, crypto,
ou 2 allers-retours codeur↔validateur sans résolution → Opus effort haut, sur le périmètre
minimal du problème uniquement.

### Règles de branche et de commit

- `main` est toujours vert (build + tests passent). Jamais de commit direct sur `main`
  (sauf docs/ et scripts/).
- Branches : `feat/<slug>`, `fix/<slug>`. Une branche = une tâche du plan, petite.
- Messages : conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`), en anglais,
  une ligne + corps bref si nécessaire.
- Chaque merge dans `main` coche la tâche correspondante dans le tableau §5 (commit `docs:`).

## 4. Checklist du validateur

1. Build et tests EN LOCAL d'abord (`cmake -S . -B build -G Ninja && cmake --build build`),
   sans erreur ni warning nouveau. La CI GitHub Actions est le second filet, pas le premier.
2. `ctest --test-dir build` : 100 % des tests passent en local, puis CI verte sur la branche.
3. Le diff correspond au périmètre de la tâche (rien de hors-sujet).
4. Nouvelle logique = nouveaux tests (obligatoire pour tout code CRDT/sync).
5. Pas de secret, pas de dépendance ajoutée sans mention dans PLAN.md.

## 5. Phases et tâches

Convention effort : **min** = Haiku sans réflexion étendue · **std** = Sonnet effort normal ·
**haut** = Opus/Fable réflexion étendue.

| # | Tâche | Codeur | Effort | État |
|---|---|---|---|---|
| 0.1 | Rédiger `docs/SPEC.md` : format des opérations CRDT, résolution de conflits, tombstones, protocole relais, format du QR d'appairage, stratégie notifications Android | Opus/Fable | haut | ✅ |
| 1.1 | Squelette Qt6/CMake (app QML minimale qui build desktop + Android), CI GitHub Actions build+test | Haiku (boilerplate) | min | ✅ |
| 1.2 | Modèle de données + persistance SQLite selon SPEC.md | Sonnet | std | ✅ |
| 2.1 | Implémentation CRDT + tests unitaires exhaustifs (conflits concurrents, tombstones, N participants, fuzzing léger de séquences d'ops) | Sonnet | std | ✅ |
| 2.2 | Revue dédiée des cas limites du merge CRDT | Opus | haut | ✅ |
| 3.1 | Écrans QML : liste des listes, items (ajouter/cocher/supprimer), création de liste | Haiku (boilerplate) | min | ✅ |
| 3.2 | Branchement UI ↔ C++ (QAbstractListModel), indicateur hors-ligne | Sonnet | std | ✅ |
| 4.1 | Client relais : QWebSocket, publish/subscribe, reconnexion, file d'attente hors-ligne | Sonnet | std | ✅ |
| 4.2 | Chiffrement E2E + génération/lecture du QR d'appairage (suivre SPEC.md à la lettre) | Sonnet | std | ✅ |
| 4.3 | Découverte LAN (mDNS) pour sync locale instantanée — **reporté v2** (bonus : la sync relais couvre le besoin) | Haiku (boilerplate) | min | ⏸ |
| 5.1 | Intégration bout-en-bout : modif → op CRDT → chiffré → relais → merge distant ; tests d'intégration multi-instances | Sonnet | std | ✅ |
| 6.1 | Déploiement Android : manifest, permissions, icônes, notifications à l'ouverture | Haiku/Sonnet | min/std | ✅ (smoke test téléphone à faire) |
| 6.2 | README public, captures, instructions de build | Haiku | min | ✅ |
| 7.1 | Passe UX + correctifs de fond : thème sombre, écrans refondus, clé de liste manquante, publication des écritures locales, scan QR caméra, deep link `colotaches://` | Opus | haut | ✅ |

Ordre strict : 0 → 1 → 2 (tests CRDT blindés **avant** tout réseau) → 3/4 en parallèle → 5 → 6.

## 6. Règles d'économie de tokens (toutes sessions)

- Lire PLAN.md + SPEC.md + les fichiers de la tâche. Rien d'autre.
- Tâches petites : si un diff dépasse ~400 lignes, découper.
- Le validateur lit le diff, pas les fichiers entiers.
- Pas de refactoring opportuniste hors du périmètre de la tâche.
- Toute décision nouvelle se consigne dans SPEC.md ou PLAN.md (une fois), jamais re-débattue.

## 7. REPRISE (état au 14/07/2026)

**L'APK est construit par la CI, plus besoin du kit Android en local.** Le job `android` de
`.github/workflows/ci.yml` produit `colotaches-arm64.apk` **signé** (clé de debug) à chaque push :
onglet *Actions* → artefacts du run. Le build local reste possible :
`scripts/setup-android.sh` (~7 Go, une fois) puis `scripts/build-android.sh`.

**Releases** : un tag `v*` (`git tag -a v0.1.0 -m "…" && git push origin v0.1.0`) déclenche
`.github/workflows/release.yml`, qui publie l'APK dans les Releases GitHub. `versionName` = le
tag, `versionCode` = nombre de commits (entier croissant, exigé par Android).

**Bugs de fond corrigés pendant la passe UX 7.1** (ils rendaient l'app inopérante en vrai, alors
que les tests passaient — ceux-ci câblaient eux-mêmes ce que l'app ne câblait pas) :
1. `ItemModel::localChanged` n'était connecté à rien et `registerItemModel` jamais appelé :
   aucune écriture locale n'était publiée, aucun événement distant ne rafraîchissait l'écran.
   `AppController` possède désormais l'`ItemModel` et fait le câblage.
2. `createList()` laissait la clé de liste **vide** : URI d'appairage rejetée (32 octets exigés)
   et chiffrement/canal identiques pour tout le monde. → `net::generateListKey()`.
3. Une liste créée après le démarrage n'était souscrite qu'au lancement suivant.

Vérifié bout-en-bout avec deux instances desktop sur les vrais relais : B rejoint par lien,
récupère l'historique, et reçoit en direct un article ajouté sur A.

Reste à faire (hors code) : **smoke test sur téléphone** — installer l'APK, vérifier le scan du
QR entre les deux appareils et l'arrivée d'une notification.

Limite v1 assumée (SPEC §8) : app fermée = pas de réception. Le rattrapage se fait au lancement
et au retour au premier plan.
