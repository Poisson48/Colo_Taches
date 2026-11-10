# Colo Sync — transport hybride (relais + P2P)

> État : v0.32.0 (Course) / v0.8.0 (Tâches). Ce document décrit le fonctionnement
> cible ; il remplace `docs/P2P-MIGRATION.md` comme référence d’architecture.

## 1. Deux serveurs indépendants

| Réglage | Rôle | Défaut |
|---|---|---|
| **Relais Nostr** (`wss://…`) | stockage durable des deltas/snaps/vignettes chiffrés | `wss://colo-apps.les-crevettes-cevenoles.fr` |
| **Serveur P2P** (`https://…`) | signaling, credentials TURN, `/config` | `https://colo-apps.les-crevettes-cevenoles.fr` |

On peut utiliser :
- relais Colo + P2P Colo ;
- relais self-host + P2P Colo ;
- relais Colo + P2P self-host ;
- les deux self-host.

Le serveur P2P n’est **pas** un serveur de données : il ne stocke aucune liste.
Il expose :

| Endpoint | Usage |
|---|---|
| `WS /signal` | découverte des pairs + SDP/ICE |
| `GET /config` | protocole, apps autorisées, URLs ICE |
| `GET /turn-credentials` | credentials TURN éphémères (HMAC, TTL 12 h) |
| `GET /healthz` | état local (nginx seulement) |

## 2. Modes de synchronisation

| Mode | Comportement |
|---|---|
| **Hybride (défaut)** | publie au relais **et** aux pairs ; P2P pour la vitesse |
| Relais seul | ancien comportement, utile en dépannage |
| P2P seul | expérimental ; aucune durabilité relais |

La migration des installations existantes bascule une fois vers Hybride
(`syncModeV2Migrated`). L’utilisateur peut revenir à « Relais seul ».

## 3. Ack et outbox

- Chaque écriture locale est d’abord dans l’outbox SQLite.
- `AckKind::RelayDurable` (OK / duplicate / stored) est le **seul** ack qui
  supprime une ligne d’outbox.
- `AckKind::PeerDelivered` marque une remise P2P, mais ne vide pas l’outbox en
  hybride.
- `AckKind::Rejected` conserve la ligne et remonte l’erreur.
- **Aucune purge des non-acquittés** : une entrée sans ack reste, et sera
  republiée. Seules les entrées dont la liste a été supprimée sont purgées.

## 4. Snapshots et rattrapage

À l’ouverture d’un data channel, chaque côté envoie `subscribe`. Le pair répond
avec un snap ciblé + les vignettes référencées. Un nouvel appareil peut donc
remplir une liste même si le relais est momentanément indisponible, tant qu’un
pair possède l’état.

## 5. Photos : vignette durable, HD P2P

| Variante | Taille | Transport | Durabilité |
|---|---|---|---|
| Vignette | 1024 px, 20 Ko (Course) / 30 Ko (Tâches) | relais + P2P | durable |
| HD | 2048 px, JPEG 85, ≤ 2 Mo | **P2P uniquement** | best-effort |

- Le champ CRDT `image` ne référence que la vignette ; les anciens clients
  restent compatibles.
- La HD est stockée dans `images.hd_data`, liée au même sha.
- Le plein écran appelle `requestHd(sha)` → `want_image` ; le pair qui l’a
  répond, sinon la vignette reste affichée.
- `publishSnap` et le rattrapage ne re-sèment que les vignettes.
- **Limite** : si aucun pair n’a reçu la HD et que l’appareil source disparaît,
  la HD est perdue ; la vignette reste. Pour garantir la HD, il faudrait un
  stockage serveur chiffré — hors de cette version.

## 6. TURN

TURN est obligatoire pour la 4G. coturn écoute 3479 (UDP/TCP) et 5350 (TLS),
avec une plage de relais 50000–50100/UDP. L’app récupère des credentials
éphémères via `/turn-credentials` ; le secret reste sur le serveur.

Ports à ouvrir sur la box/routeur :

| Port | Proto |
|---|---|
| 3479 | UDP + TCP |
| 5350 | TCP |
| 50000–50100 | UDP |

Le 3478 est réservé au STUN meownopoly du serveur Colo ; ne pas y toucher.

## 7. Self-host

Voir [deploy/selfhost/README.md](../deploy/selfhost/README.md) et
[deploy/server/](../deploy/server/). Les scripts sont paramétrés par
`DOMAIN`, `PUBLIC_IP`, `PRIVATE_IP`, `COLO_DATA_DIR`, `ALLOWED_APPS`.

## 8. Tests

- `tst_webrtc` : deux transports en loopback, data channel, event, ack P2P.
- `tst_sync` : relais, outbox, reconnexion.
- `tst_payload` / `tst_database` : codec image HD et stockage.
- CI : Linux, Android arm64, QML, deux apps.
