# Dimensionnement

Dernière mise à jour : **2026-11-12**. Plus de P2P/TURN/signaling : le relais
Nostr et les notifications sont les seuls points de charge.

## Hypothèses

- 1 000 utilisateurs simultanés ;
- groupes de 15 ;
- 20 listes par utilisateur ;
- propagation < 10 s ;
- événements chiffrés de quelques centaines d'octets ; vignettes limitées.

## Relais

- 1 000 WebSockets, ~20 000 abonnements ;
- un changement est rediffusé aux abonnés du canal ;
- Strfry actuel absorbe cette charge ; tester avant de dépasser quelques milliers
  de clients.

## Notifications

| Mode | Charge |
|---|---|
| Différé | jobs locaux, aucun flux push permanent |
| FCM | un appel Google par changement, rate-limit 1/5 s/topic |
| UnifiedPush | un POST par endpoint et par changement |

Le broker Node doit rester derrière nginx avec rate-limit. Pour de gros volumes,
limiter les endpoints et regrouper les envois.

## Stockage

- relais : deltas + snapshots chiffrés, rétention pilotée par les acks ;
- vignettes limitées, pas de HD ;
- registre UnifiedPush : JSON, quelques dizaines d'octets par appareil.

## Monter en charge

1. Mesurer WebSockets, abonnements, latence p95, volume/jour.
2. Ajouter des relais et configurer plusieurs URLs.
3. Séparer relais, broker et ntfy sur des machines distinctes si nécessaire.
4. Ne pas réintroduire P2P/TURN : le relais reste le chemin unique.
