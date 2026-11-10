# Proposition : serveur de synchronisation chiffré (hub)

> Cible : 1 000 utilisateurs simultanés, groupes de 15, 20 listes par personne,
> propagation < 10 s. **Décision 2026-11-10 : le P2P/WebRTC/TURN est retiré.**
> Le relais serveur chiffré est le chemin unique ; les photos HD ne sont plus
> synchronisées.

## 1. Principe

Un serveur de synchronisation reçoit des **événements chiffrés** (deltas, snaps,
vignettes), les pousse aux membres de la liste, et suit les accusés de réception
par appareil. Il ne peut pas déchiffrer le contenu.

```
User1 ──(event chiffré + seq)──► Colo Sync Server ──► User2…User15
                                      │
                                      ├─ roster de la liste (device IDs)
                                      ├─ watermarks d’ack par device
                                      └─ demandes de snapshot (orchestration)
```

Le serveur ne stocke **aucune clé** et ne lit aucun payload.

## 2. Ce que le serveur doit connaître

Pour savoir « qui a reçu quoi », il faut un **registre par liste** :

- `listId` opaque (dérivé de la clé, comme aujourd’hui) ;
- appareils membres (`deviceId`) et clé publique de signature ;
- preuve d’appartenance à l’ajout (HMAC/token dérivé de la clé de liste) ;
- dernier `seq` acquitté par appareil ;
- date de dernière activité.

Le serveur voit donc **des métadonnées** : liste, appareils, tailles, horodatages.
Il ne voit ni les articles, ni les quantités, ni les photos.

## 3. Protocole

### Publication

```json
{
  "type": "publish",
  "list_id": "…",
  "event_id": "…",
  "kind": "delta|snap|thumb",
  "ciphertext": "…",
  "sender": "device-…",
  "sig": "…"
}
```

Le serveur :

1. vérifie la signature et l’appartenance ;
2. attribue un `seq` monotone par liste ;
3. stocke l’événement ;
4. pousse `event` aux appareils membres connectés ;
5. attend les `ack`.

### Ack

```json
{ "type": "ack", "list_id": "…", "seq": 42 }
```

Le client ack **après merge SQLite réussi**, pas à la réception réseau. Un ack
perdu est rejoué ; il est idempotent.

### Rattrapage

```json
{ "type": "subscribe", "list_id": "…", "after_seq": 41 }
```

Le serveur envoie les événements `seq > 41` encore disponibles. S’ils ont été
supprimés, il envoie `gap { from_seq, to_seq }` et le dernier snapshot connu.

## 4. Suppression et rétention

Le serveur ne supprime pas « quand tout le monde a reçu » au sens vague. Il
utilise un **watermark** :

```
minAck = min(last_ack[device]) pour les appareils actifs
delete events where seq <= minAck - grace
```

- `active` = vu dans les **30 derniers jours** ;
- `grace` = quelques jours pour les retards/retries ;
- un appareil inactif depuis 30 jours est marqué `gap` et **ne bloque plus** la
  suppression ;
- un appareil qui revient reçoit `gap` + snapshot.

### Un nouvel appareil n’a pas besoin de l’historique

C’est vrai pour cette app : il n’y a pas d’écran d’historique. Un nouvel appareil
a besoin de **l’état courant**, pas des deltas passés. Un appareil resté 31 jours
hors ligne a encore son ancienne liste : il peut l’afficher, puis demander ce qui
lui manque. Il n’a pas « plus rien ».

Le serveur n’a donc pas besoin de garder un snapshot en permanence. Il garde :

- les deltas tant que tous les appareils actifs n’ont pas ack ;
- un `gap` par appareil s’il a dû supprimer des deltas ;
- une **demande de snapshot** tant qu’un appareil n’a pas récupéré l’état.

### Qui renvoie l’état courant ?

Quand le serveur a supprimé les deltas, il ne peut pas les renvoyer. Il
**orchestre** :

1. l’appareil en retard se connecte avec `last_ack` ;
2. le serveur constate le `gap` et enregistre une demande ;
3. il choisit un appareil **en ligne** avec `last_ack >= gap.to_seq` (le plus
   récent) ;
4. il lui envoie `snapshot_request { list_id, to_seq }` ;
5. cet appareil chiffre son état courant, le signe et l’envoie ;
6. le serveur le relaie au demandeur (et peut le garder temporairement jusqu’à
   l’ack, puis l’oublier) ;
7. le demandeur fusionne le snapshot avec son état local + son outbox, puis ack.

Si aucun pair n’est en ligne, la demande reste en attente. Elle est satisfaite
au prochain passage d’un pair complet. C’est le prix du « pas de stockage
permanent » : la durabilité dépend d’au moins un pair.

Le « flag non reçu » devient un `gap` par appareil : à la reconnexion, le client
sait qu’il lui manque `[from_seq, to_seq]` et demande le snapshot.

## 5. Le « serveur demande à un user de renvoyer »

C’est la procédure principale quand les deltas sont supprimés, pas un simple
fallback :

1. le serveur choisit un appareil actif avec `last_ack >= gap.to_seq` ;
2. il lui envoie `snapshot_request { list_id, to_seq }` ;
3. l’appareil chiffre son état courant et le publie comme `snap` ;
4. le serveur le relaie au demandeur.

Il faut un timeout, plusieurs candidats, et un fallback « réessayer plus tard ».
Un snapshot serveur permanent est plus simple et plus fiable, mais consomme du
stockage ; le snapshot à la demande est plus économe, au prix d’une dépendance à
un pair en ligne.

## 6. Dimensionnement 1 000 utilisateurs

- 1 000 WebSockets, 67 groupes, 15 membres par groupe.
- 20 000 abonnements (1 000 × 20 listes) : le serveur peut les indexer en
  mémoire ; la base stocke les watermarks (`device × list`), pas une matrice
  événement × appareil.
- Un événement par modification ; fan-out à 15 membres ; < 1 s sur le serveur.
- Le serveur n’a plus besoin de TURN ni de maillage WebRTC pour ce chemin.
- Postgres (ou SQLite pour un petit déploiement) + Redis/pub-sub pour plusieurs
  instances ; WebSocket derrière un load balancer avec sticky sessions.
- Objets : vignettes et snapshots **temporaires** dans un stockage chiffré ;
  quotas par liste.

## 7. Sécurité

- Chiffrement E2E existant (XChaCha20-Poly1305) ; le serveur ne voit que des
  blobs.
- Signature d’appareil (Ed25519) sur chaque événement.
- Preuve d’appartenance dérivée de la clé de liste ; la clé n’est jamais envoyée.
- Rate-limit, taille max, quota par liste, anti-replay par `event_id`.
- Le serveur voit les métadonnées (membres, horaires, tailles) : à documenter.
- Suppression logique ≠ effacement physique (SSD) : le dire clairement.

## 8. Migration depuis le système actuel

1. Garder le relais Nostr comme fallback pendant la transition.
2. Ajouter le serveur hub en **dual-write** : les clients publient au hub et au
   relais.
3. Les clients lisent le hub en priorité ; le relais sert de secours.
4. Une fois 1 000 utilisateurs validés, le hub devient le chemin principal ;
   le P2P/TURN reste optionnel pour les petits groupes.
5. Les anciens clients continuent via le relais.

## 9. Ce que cela change pour la v0.32.0

La v0.32.0 est désormais **relais seul** : plus de P2P, plus de TURN, plus de
photos HD synchronisées. Le hub décrit ici est la cible pour 1 000 utilisateurs ;
il peut être déployé derrière le même relais/API sans maillage.

## 10. Décisions à prendre

- Faut-il des comptes utilisateurs, ou seulement des appareils + preuve de liste ?
- Accepte-t-on que la récupération dépende d’au moins un pair en ligne ?
- Que fait-on si aucun pair complet n’est en ligne pendant longtemps ?
- Garde-t-on un snapshot de secours pour les listes dormantes ?
- Les vignettes sont-elles stockées sur le serveur (oui recommandé) et les HD
  restent-elles P2P (oui) ?
- Quotas : taille par liste, nombre d’appareils, débit.
