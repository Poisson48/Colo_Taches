# Dimensionnement : combien d’appareils, de listes, de données ?

> **Décision 2026-11-10** : le P2P/TURN est retiré. Les calculs de maillage et les
> options A/B/C ci-dessous sont historiques. Pour 1 000 utilisateurs, voir
> [SYNC-SERVER.md](SYNC-SERVER.md) : relais serveur chiffré, watermarks, snapshot
> à la demande, pas de ports TURN.

## Ce qui compte n’est pas le nombre d’appareils

C’est le nombre de **connexions WebRTC simultanées** et le nombre d’allocations TURN
actives. Un appareil peut avoir 15 listes sans ouvrir 15 connexions s’il est seul
dans ces listes.

### P2P en maillage

Pour une liste avec `N` appareils en ligne, le maillage crée :

- `N × (N - 1) / 2` connexions pour toute la liste ;
- `N - 1` connexions par appareil.

Exemples :

| Appareils sur une liste | Connexions totales |
|---|---|
| 2 | 1 |
| 4 | 6 |
| 8 | 28 |
| 20 | 190 |
| 500 | 124 750 |

Un maillage complet à 500 appareils est **impossible** en pratique (signaling,
CPU, NAT, TURN). Ce n’est pas un réglage de ports : c’est une limite d’architecture.

### Limites actuelles

- `MAX_PEERS_PER_ROOM=8` : au-delà, les appareils suivants restent sur le relais ;
- `MAX_ROOMS_PER_CLIENT=32` ;
- une `PeerConnection` par couple **(appareil, liste)** ;
- relais Nostr = chemin durable pour tout le monde, P2P = accélération locale.

Le serveur refuse proprement les pairs au-delà de la limite : l’app continue en
relais, sans perte. C’est le mode à utiliser pour les grands groupes.

## Ports TURN

Coturn alloue **un port de relais par allocation TURN active**, pas un port par
paquet ni un port par appareil au repos. La taille de la plage = nombre maximum
d’allocations simultanées.

Avec la plage actuelle `50000–50019`, on a **20 allocations simultanées**. C’est
dimensionné pour une famille/coloc (2–5 appareils, quelques listes). Ce n’est pas
dimensionné pour 500 appareils.

Pour un plus grand déploiement, deux choix :

1. **Recommandé : relais seul / P2P limité aux petits groupes.**
   - `syncMode=0` (relais seul) pour les grandes listes ;
   - ou plafonner `MAX_PEERS_PER_ROOM` à 2–4 ;
   - TURN ne sert qu’aux quelques connexions P2P, la plage reste petite.
2. **P2P à grande échelle : SFU / cluster TURN.**
   - plusieurs IP publiques, plage de ports large, signaling réparti ;
   - ce n’est plus la même architecture ; à chiffrer séparément.

## 500 appareils × 15 listes : ce qu’il faut faire

- Si les 15 listes sont **petites** (2–4 appareils par liste) : le P2P reste local
  et la plage de 20 ports peut suffire ; sinon passer à 100 ports.
- Si les 500 appareils sont **sur les mêmes listes** : ne pas utiliser le maillage.
  Passer ces listes en **relais seul** (ou plafonner les pairs P2P), et laisser
  Strfry absorber le fan-out.
- Si le besoin est vraiment « 500 utilisateurs en P2P simultané » : revoir
  l’architecture (SFU/cluster), pas seulement la box.

## Réglages serveur

```ini
MAX_ROOMS_PER_CLIENT=32
MAX_PEERS_PER_ROOM=8
```

Pour un déploiement plus large, baisser `MAX_PEERS_PER_ROOM` (2–4) est plus sûr
que d’ouvrir des milliers de ports TURN. Le relais reste le chemin durable.

---

# Cas cible : 1 000 utilisateurs, groupes de 15, 20 listes chacun

## Hypothèses de calcul

- 1 000 utilisateurs ÷ 15 = **67 groupes**.
- Chaque groupe partage **20 listes** entre ses 15 membres.
- Tous les membres d’un groupe sont en ligne au même moment.
- Une liste = un canal ; un changement doit atteindre les 14 autres membres en < 10 s.

## Pourquoi l’architecture actuelle ne tient pas

Le code actuel ouvre **une `PeerConnection` par couple (appareil, liste)**.
Pour un utilisateur :

```
14 pairs × 20 listes = 280 PeerConnections
```

Pour un groupe :

```
20 listes × (15 × 14 / 2) = 2 100 connexions
```

Pour 67 groupes :

```
2 100 × 67 = 140 700 connexions
```

C’est impossible : CPU, radio, batteries, signaling, NAT et TURN.

Même en multiplexant toutes les listes sur **une seule connexion par paire** :

```
15 × 14 / 2 = 105 connexions par groupe
105 × 67 = 7 035 connexions
14 connexions par utilisateur
```

C’est faisable, mais il faut changer trois choses : le multiplexage, le
signaling de groupe, et TURN.

## Solution A — Maillage complet multiplexé + cluster TURN (P2P pur)

- Une seule `PeerConnection` par paire ; les 20 listes passent dans un canal
  unique, avec un `channelTag` par message.
- Un seul salon de signaling par groupe (15 membres), pas 20 salons par liste.
- TURN : jusqu’à **7 035 allocations** si tout le monde est derrière un NAT
  symétrique. Il faut plusieurs serveurs TURN avec IPv4 publique, par exemple
  4 × 2 000 ports. La NB6 ne peut pas porter 7 000 redirections.
- Signaling : 1 000 WebSockets, 67 salons ; Redis/pub-sub et plusieurs instances.
- Avantages : toutes les paires sont directes, latence minimale, aucun contenu
  serveur.
- Inconvénients : 14 connexions par appareil, pic de connexion au démarrage,
  batterie, 7 000 allocations TURN, bandwidth HD.
- Pour tenir 10 s : connexions **paresseuses** (uniquement les listes actives) et
  reprise par le relais pendant que le P2P s’établit.

## Solution B — Maillage partiel + supernodes (recommandé pour mobile)

- Chaque utilisateur se connecte à **4 à 6 voisins** du groupe, pas aux 14.
- Les événements sont **gossipés** de proche en proche ; le relais reste le filet
  durable.
- Supernodes : les appareils stables (desktop, secteur) couvrent les appareils
  mobiles.
- Connexions : ~45 par groupe, ~3 000 au total ; TURN ~3 000 au pire.
- Avantages : beaucoup moins de connexions, batterie, TURN réduit.
- Inconvénients : logique de forwarding, déduplication, ack, boucles ; tous les
  pairs ne sont pas directement connectés.
- C’est le meilleur compromis « P2P + 1 000 utilisateurs + mobile ».

## Solution C — SFU de groupe (relais WebRTC)

- Chaque utilisateur ouvre **une** connexion WebRTC vers un SFU régional ;
  le SFU redistribue aux 14 autres.
- 1 000 connexions, pas 7 000 ; 10 s très large ; mobile-friendly.
- Le SFU voit les blobs chiffrés mais ne peut pas les déchiffrer.
- Inconvénient : ce n’est plus du P2P direct, et il faut de la bande passante
  serveur. À utiliser si « P2P » est un moyen et pas une obligation absolue.

## Solution D — Relais d’abord, P2P opportuniste

- Le relais Nostr reste le chemin principal ; le P2P ne sert qu’aux petits
  groupes actifs.
- 1 000 clients WebSocket, ~20 000 abonnements ; Strfry peut le faire, à valider
  par un test de charge.
- Aucun problème de ports TURN : seuls les groupes P2P en consomment.
- C’est la solution la plus simple pour garantir < 10 s et zéro perte.

## Recommandation

1. **À court terme** : garder la v0.32.0 famille (2–8 appareils par liste),
   plafonner `MAX_PEERS_PER_ROOM` et documenter le mode relais seul.
2. **Pour 1 000 utilisateurs** : viser **B + relais** si le P2P est obligatoire,
   ou **C + relais** si le SLA 10 s primait sur le « tout P2P ».
3. **Ne pas** se contenter d’ouvrir 7 000 ports sur la NB6 : déplacer TURN sur
   des VPS publics, avec plusieurs IP et une plage de ports par instance.
4. **Avant tout** : implémenter le multiplexage (une connexion par paire) et le
   signaling de groupe ; sans ça, aucun des scénarios 1 000 utilisateurs ne tient.
5. **Tester** : 1 000 clients WebSocket simulés, 7 000 connexions, matrice TURN,
   mesure p95 de propagation sur 20 listes.
