# Serveur de synchronisation

Dernière mise à jour : **2026-11-12**. Architecture : relais Nostr + broker de
notifications. **Pas de P2P, pas de TURN, pas de WebRTC.**

```text
Appareil A ── wss ──► Strfry ── wss ──► Appareil B
     └── POST /push ──► colopush ── FCM / UnifiedPush
```

| Composant | Rôle | Port / URL |
|---|---|---|
| Strfry | relais Nostr kind 4545 | `127.0.0.1:7777`, nginx `/` |
| colopush | FCM + UnifiedPush | `127.0.0.1:7780`, nginx `/push` |
| ntfy | notifications différées / distributeur | `127.0.0.1:8077`, nginx `/ntfy/` |
| nginx | TLS et reverse proxy | `:443` |

## Relais

- `wss://colo-apps.les-crevettes-cevenoles.fr` ;
- kind `4545` uniquement, tag `t=channelTag` ;
- blobs chiffrés XChaCha20-Poly1305 ;
- `maxEventSize` 256 Ko (photos de tâches) ;
- aucun accès en clair aux listes.

## Broker

- `/push` FCM data-only + endpoints UnifiedPush ;
- `/config` capacités ;
- secrets FCM dans `/data/leo/colo-apps/secrets/fcm-service-account.json` ;
- registre UnifiedPush dans
  `/data/leo/colo-apps/push/endpoints.json`.

## Déploiement

Voir [Colo_Course/deploy](https://github.com/Poisson48/Colo_Course/tree/main/deploy)
pour les scripts de référence. Vérifications :

```bash
curl -fsS https://colo-apps.les-crevettes-cevenoles.fr/push
curl -fsS https://colo-apps.les-crevettes-cevenoles.fr/config
systemctl is-active colopush nginx
```

Les volumes Strfry/ntfy sont à sauvegarder. Si tous les appareils disparaissent,
une liste peut être perdue : le relais ne conserve pas éternellement les deltas.

Voir [PROTOCOL.md](../PROTOCOL.md) et [NOTIFICATIONS.md](NOTIFICATIONS.md).
