# Protocole Colo Tâches

Référence normative du protocole de **Colo Tâches**. Version **1**.
Dernière mise à jour : **2026-11-12**.

> Ce document remplace l'ancienne `docs/SPEC.md`. Produit actuel :
> **relais Nostr chiffré uniquement, sans P2P, sans TURN, sans WebRTC.**

## 1. Principes

- Copie locale complète (SQLite) sur chaque appareil.
- Une liste = une clé symétrique 32 octets = un canal chiffré.
- Relais Nostr store-and-forward : il ne voit que des blobs.
- CRDT delta-state : merge idempotent, commutatif, associatif.
- Aucun compte : seul un `deviceId` par installation.
- Notifications : réveil `sync` sans contenu, jamais le nom d'une tâche.

## 2. Identifiants

| Nom | Format | Rôle |
|---|---|---|
| `listId` | 16 octets base64url | liste |
| `itemId` / `stepId` | UUID v4 | tâche / étape |
| `deviceId` | UUID v4 | installation |
| `listKey` | 32 octets | clé de liste |
| `channelTag` | SHA256 tronqué, 32 hex | canal Nostr |
| `ver` | `[lamport, deviceId]` | version LWW |

```text
channelTag = hex(SHA256("colo-course/v1/channel" || listKey))[0..31]
nostrSeed  = SHA256("colo-course/v1/nostrkey" || listKey)
```

Le keypair Nostr est dérivé de la clé de liste ; les membres partagent la même
identité de publication.

## 3. Modèle CRDT

Champ versionné : `[lamport, deviceId]`. Merge :

```text
si ver_recu > ver_local : adopter valeur + version
sinon : ignorer
```

### Tâche / étape

Champs versionnés : `name`, `qty`, `note`, `aisle` (catégorie), `image`, `due`,
`dueNote`, `order`, `done`, `del`.

Métadonnées :

- `created`, `by` ;
- `parent` : `stepId` de la tâche parente (étape) ;
- `doneAt` : date de cochage, satellite de `done` ;
- `touched` : local uniquement, jamais synchronisé.

`image` contient les SHA-256 des blobs JPEG séparés par des espaces. Les blobs
circulent dans des événements `img` séparés et sont adressés par contenu.

`del=true` est un tombstone ; suppression d'une tâche = suppression logique de
ses étapes.

### Liste

`title` versionné ; `members` (deviceId → displayName) informatif ; `kind` de
liste immuable ; tri local.

### Lamport

Compteur par liste, persisté. Modification locale `+1` ; réception :
`clock = max(clock, lamports reçus)`.

### Tombstones

Purge locale après :
- lamport de `del` plus vieux de `GC_AGE = 10000` que le lamport courant ;
- et `touched` de plus de 30 jours.

Un appareil très en retard peut ressusciter un élément purgé ; accepté.

## 4. Transport Nostr

- Relais par défaut : `wss://colo-apps.les-crevettes-cevenoles.fr`.
- Kind `4545`, tag `["t", channelTag]`.
- Souscription `{"kinds":[4545],"#t":[channelTag],"since":<lastSync-3600>}`.
- Contenu : `base64(nonce || ciphertext)`.
- XChaCha20-Poly1305, nonce 24 octets, AD = channelTag.
- Événements `delta`, `snap`, `img` ; snapshots trop gros scindés.

## 5. Payloads

```json
{
  "v": 1,
  "t": "delta",
  "list": "<listId>",
  "by": "<deviceId>",
  "title": ["Maison", [3, "<deviceId>"]],
  "items": [
    {
      "id": "<uuid>",
      "created": 1721000000000,
      "by": "<deviceId>",
      "doneAt": 0,
      "f": {
        "name": ["Repeindre le volet", [12, "<deviceId>"]],
        "aisle": ["Bricolage", [12, "<deviceId>"]],
        "due": [1721088000000, [12, "<deviceId>"]],
        "done": [false, [12, "<deviceId>"]],
        "del": [false, [12, "<deviceId>"]]
      }
    }
  ]
}
```

- `"t"` : `delta`, `snap`, `img`.
- `"snap"` ajoute `members` et l'état complet.
- `"parent"` identifie l'étape.
- `"image"` contient les SHA-256 des photos.
- Champs inconnus ignorés ; champ connu absent = `[0, ""]`.
- `"v" != 1` : événement ignoré.

Publication :

- delta à chaque modification (débounce 300 ms) ;
- snap après ≥ 100 deltas ou 7 jours ;
- nouvel appareil : souscription sans `since`, `limit: 500`, merge de tout ;
- hors ligne : outbox, flush au retour réseau.

## 6. Appairage

```text
colotaches://join/1/<listId>/<base64url(listKey)>/<urlencode(titre)>
```

QR code ou lien. Le lien contient la clé : ne pas le publier. Révocation non
supportée : recréer la liste pour exclure quelqu'un.

## 7. Notifications

Le contenu d'une liste ne circule jamais dans une notification.

```http
POST /push
{"app":"colotaches","topic":"colo-<32 hex>"}
→ 202 | 400 | 429 | 502
```

- FCM data-only ou UnifiedPush (endpoint par appareil) ;
- `/config` expose les capacités ;
- côté app : `notif.mode` (`off|deferred|immediate`), `notif.provider`
  (`auto|fcm|unifiedpush`), `notif.alert` (`notify|silent`), `notif.signal`
  (`sound|vibrate|visual|none`) ;
- `signal = none` : aucune notification, synchro silencieuse.

Voir [docs/NOTIFICATIONS.md](docs/NOTIFICATIONS.md).

## 8. Comportement

1. Modification locale → merge → delta chiffré → outbox → relais.
2. Réception → déchiffrement → merge → notification si autorisée.
3. Retour réseau → flush outbox → rattrapage `since`.
4. App au premier plan : WebSocket Qt.
5. Arrière-plan : FCM/UnifiedPush (immédiat) ou JobScheduler (différé).
6. App tuée : notification possible ; fusion à froid non garantie, l'ouverture
   de l'app rattrape l'état.

## 9. Sécurité et limites

- Le relais voit listId, channelTag, tailles, horodatages, IP.
- Il ne voit ni titres, ni tâches, ni photos.
- FCM implique Google ; UnifiedPush permet de l'éviter.
- Perte possible si tous les appareils disparaissent.
- Photos : vignettes uniquement, pas de HD.
