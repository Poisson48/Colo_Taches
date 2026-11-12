# Notifications — état actuel

Dernière mise à jour : **2026-11-12**. Remplace les anciens plans P2P/TURN.

## Modes proposés

| Choix utilisateur | `notif.mode` | `notif.alert` | Transport |
|---|---|---|---|
| M'avertir immédiatement | `immediate` | `notify` | FCM ou UnifiedPush |
| M'avertir par périodes (sans Google) | `deferred` | `notify` | ntfy / JobScheduler |
| Synchroniser sans notification | `deferred` | `silent` | ntfy / JobScheduler |
| Ne rien faire | `off` | — | aucun |
| Signal **Aucun** sur immédiat | `immediate` | `silent` | FCM ou UnifiedPush |

Le signal vaut `sound`, `vibrate`, `visual` ou `none`. `none` n'affiche aucune
notification : la mise à jour est silencieuse.

## Fournisseurs

### Google FCM

- message **data-only** `{app:"colotaches", kind:"sync"}` ;
- abonnement aux topics `colo-<channelTag>` ;
- nécessite les services Google et l'accord explicite de l'utilisateur ;
- l'UI affiche l'avertissement : Google reçoit un signal générique, jamais le
  contenu des listes.

### UnifiedPush (sans Google)

- distributeur externe, testé avec l'app **ntfy** ;
- l'app enregistre son endpoint auprès du broker :
  `POST /push {"action":"register","app":"colotaches","endpoint":"https://…"}` ;
- le broker envoie `sync` à l'endpoint ;
- le distributeur ntfy doit être installé et **exempté d'optimisation batterie** ;
- aucun service Colo permanent : c'est le distributeur qui garde la connexion.

### Différé (sans Google)

- JobScheduler 15/30/60 min ;
- réveil ntfy possible pour les changements ;
- pas de service permanent, meilleure autonomie.

## Broker `/push`

Service Node `deploy/push/server.mjs`, port local `7780`, exposé par nginx sur
`/push` :

```http
POST /push
{ "app": "colocourse|colotaches", "topic": "colo-<32 hex>" }
→ 202 | 400 | 429 | 502
```

Registre UnifiedPush dans `/data/leo/colo-apps/push/endpoints.json` :

```json
{"colocourse":["https://ntfy.sh/up…"],"colotaches":["https://ntfy.sh/up…"]}
```

Réponses utiles :

```json
{"ok":true,"fcm":true,"unifiedpush":1}
```

`GET /config` est exposé publiquement et renvoie les capacités FCM/UnifiedPush.
`GET /push` reste le health simple.

## Limites connues

- **Synchro à froid** : la notification arrive app tuée ; la fusion des données à
  froid n'est pas garantie sur tous les appareils. Le mode différé ou l'ouverture
  de l'app rattrape l'état. C'est le compromis retenu pour éviter un service
  permanent.
- **UnifiedPush testé avec ntfy.sh** : le broker et les endpoints sont sur le
  serveur Colo, mais le distributeur de test est ntfy.sh. Pour du 100 %
  auto-hébergé, il faut exposer ntfy sur une URL racine ou un sous-domaine avec
  certificat ; l'app ntfy refuse actuellement une URL contenant `/ntfy`.
- **FCM** : si l'utilisateur choisit FCM, il accepte le passage par Google.
- **Pas de contenu** : aucune notification ne contient un nom de liste ou un
  article.

## Batterie

| Mode | Coût |
|---|---|
| Différé | le plus faible : aucun service permanent |
| Immédiat FCM | réveils par changement, Play Services toujours actif |
| Immédiat UnifiedPush | connexion permanente du distributeur ntfy (~1 %/jour) |
| Signal Aucun | même coût que le transport, sans affichage |

## Réglages persistés

```text
notif.mode       off | deferred | immediate
notif.provider   auto | fcm | unifiedpush
notif.alert      notify | silent
notif.signal     sound | vibrate | visual | none
notif.interval   15 | 30 | 60
```

## Test manuel

1. Réglages → Notifications → choisir un mode et un moteur.
2. Tester la notification → un retour est affiché.
3. Fermer l'app, envoyer un push :

```bash
curl -fsS -X POST https://colo-apps.les-crevettes-cevenoles.fr/push \
  -H 'Content-Type: application/json' \
  -d '{"app":"colotaches","topic":"colo-0123456789abcdef0123456789abcdef"}'
```

4. Vérifier la notification ou son absence (signal `none`).
