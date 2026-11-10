# Batterie, notifications et synchro en arrière-plan

> Architecture cible : relais serveur chiffré, **sans P2P/TURN**. Ce document
> remplace `docs/NOTIFICATIONS.md` pour la partie batterie/notifications.

## 1. Le problème actuel

`PushService.java` tourne en **service de premier plan** et boucle toutes les
**5 secondes** sur chaque topic ntfy :

```
1 topic  : 17 280 requêtes/jour
20 listes: 345 600 requêtes/jour
```

En plus : notification permanente, radio mobile réveillée en continu, service
tué par Doze/OEM, et aucune vraie maîtrise de la consommation. C'est le principal
poste batterie de l'app en arrière-plan.

## 2. Ce qu'on veut

Trois choix explicites, sans dépendance obligatoire à Google :

| Mode | Comportement | Batterie | Latence |
|---|---|---|---|
| **Désactivées** | rien en arrière-plan | 0 | à l'ouverture |
| **Différées** (défaut) | réveil périodique 15/30/60 min, sync courte, puis stop | < 1 %/jour | jusqu'à l'intervalle |
| **Immédiates sans Google** | UnifiedPush/ntfy distributor | ~1 %/jour | < 5 s |
| **Immédiates avec Google** | FCM data-only | ~0 %/jour | < 5 s |

La synchro en arrière-plan ne doit **pas** être un processus Qt permanent ni un
WebSocket permanent. Le foreground WebSocket reste utilisé **uniquement quand
l'app est ouverte**.

## 3. Sans Google

### 3.1 Différé (recommandé, mais pas imposé)

- `JobScheduler` (ou WorkManager) périodique, minimum Android 15 min.
- Une requête légère pour l'app (broker `/pull` ou topic agrégé), pas une par
  liste.
- Le worker réveille un court `SyncWorker` (Qt ou Java) qui :
  1. récupère les deltas manquants ;
  2. merge SQLite ;
  3. affiche une notification locale si un changement concerne l'utilisateur ;
  4. s'arrête.
- Pas de notification permanente, pas de service foreground.
- Contraintes : réseau disponible, batterie pas faible, pas de WiFi-only par
  défaut. Rien n’est planifié tant que l’utilisateur n’a pas choisi un mode.
- Doze peut décaler de plusieurs heures : à afficher dans l'UI.
- Reboot : `BOOT_COMPLETED` reprogramme le job.

**Budget** : 96 réveils/jour max à 15 min, une requête chacun, < 15 Mo/mois,
< 1 % batterie sur 24 h écran éteint.

### 3.2 Immédiat sans Google : UnifiedPush

- L'app s'enregistre auprès d'un **distributor** UnifiedPush (ntfy auto-hébergé
  peut l'être).
- Le serveur envoie un message **data-only** au distributor ; l'OS réveille
  l'app, qui synchronise quelques secondes puis s'arrête.
- Aucun WebSocket permanent, aucune notification permanente.
- Repli si aucun distributor : mode Différé.
- C'est la seule option « immédiat sans Google » vraiment économe.

### 3.3 Immédiat sans Google, variante socket (dernier recours)

- Service foreground avec WebSocket ntfy + ping 90 s.
- **3–8 %/jour**, notification permanente, exemption batterie demandée.
- À proposer uniquement comme option avancée, jamais par défaut.

## 4. Avec Google : FCM

- Message **data-only**, aucun titre, aucune donnée de liste.
- L'app affiche une notification locale après avoir lu le changement chiffré.
- Le serveur `/push {app, device}` fait le fan-out FCM ; rate-limit.
- Si Play Services absent, `/config` sans `push` → option masquée, repli
  UnifiedPush/ntfy.
- Batterie ~0 %, latence < 5 s, Doze inclus.
- Le projet Firebase n'est **pas** obligatoire pour un self-host : c'est une
  commodité de l'instance Colo.

## 5. Serveur

- garder ntfy pour le différé et UnifiedPush ;
- `/push` broker optionnel pour FCM ;
- un topic par appareil ou par liste, jamais de contenu ;
- `POST /push` : 202/429, rate-limit par topic, métriques par app ;
- `/config` déclare les capacités : `push.fcm`, `push.unifiedpush`, `ntfy` ;
- aucune donnée de liste dans la notification.

## 6. UX

Écran « Notifications et arrière-plan » :

1. **Désactivées** — aucune synchro en arrière-plan.
2. **Différées (recommandé)** — intervalle 15/30/60 min, sans Google, pas de
   notification permanente.
3. **Immédiates** — fournisseur :
   - « Sans Google (UnifiedPush) » ;
   - « Google (FCM) » si disponible.
   Avertissement batterie pour le mode socket.

Afficher : dernier réveil, dernière synchro, données du mois, fournisseur actif,
bouton « Tester ». Aucune présélection : `notif.mode` reste `unset` tant que
l’utilisateur n’a pas choisi. Migration : l’ancien `pushEnabled` n’est pas
converti automatiquement ; l’ancienne veille est arrêtée et l’utilisateur choisit
une fois.

## 7. Plan de mise en œuvre

| Phase | Contenu | Durée |
|---|---|---|
| P0 | Mesurer l'existant (`batterystats`, requêtes, Doze) | 0,5 j |
| P1 | Remplacer le service 5 s par JobScheduler + réglages, sans présélection | 3 j |
| P2 | Sync courte en arrière-plan (worker qui démarre, merge, stop) | 3 j |
| P3 | UnifiedPush/ntfy immédiat sans Google | 3 j |
| P4 | FCM optionnel + `/push` + `/config` | 2 j |
| P5 | UX, métriques, tests Doze/reboot/OEM | 2 j |

**Total ~13 j**, dont P1+P2 (6 j) sans Google et sans serveur supplémentaire.
P3 peut suivre ; P4 est optionnel.

## 8. Critères d'acceptation

- Off : 0 requête, 0 Mo, 0 %.
- Différé : ≤ 96 requêtes/jour, < 15 Mo/mois, < 1 %/24 h écran éteint, aucune
  notification permanente.
- UnifiedPush/FCM : < 5 s, < 1 %/jour, aucune notification permanente.
- Doze (`force-idle`), reboot, swipe, force-stop, permission refusée, pas de
  réseau, self-host sans Google : pas de crash, repli propre.
- WebSocket foreground : fermé dès que l'app passe en arrière-plan.

## 9. Ce qu'on ne fait pas

- Pas de service Qt permanent.
- Pas de WebSocket permanent en arrière-plan.
- Pas de polling 5 s.
- Pas d'exemption batterie demandée par défaut.
- Pas de contenu de liste dans une notification.

## 10. Proposition unique, puis réglages

### Première fois

Le choix est proposé **une seule fois**, après le choix du nom (ou au premier
lancement pour une installation existante), dans une feuille non bloquante :

> **Notifications et arrière-plan**
> Comment veux-tu être prévenu des changements ?
>
> ○ Désactivé — rien en arrière-plan
> ○ Différé (recommandé) — vérification toutes les 15 min, sans Google
> ○ Immédiat — sans Google (UnifiedPush) / avec Google (FCM)
>
> [Plus tard] [Activer]

- **Aucune option n’est présélectionnée** : les trois sont vides au départ.
- « Activer » reste désactivé tant que l’utilisateur n’a pas choisi.
- « Plus tard » ou fermer la feuille marque la proposition comme vue, laisse
  `notif.mode=unset` et **ne planifie aucun travail en arrière-plan**.
- L’app reste utilisable : elle se synchronise à l’ouverture, comme en mode
  Désactivé, mais l’UI indique « Notifications non configurées ».
- Le flag `notif.onboardingV1=1` est écrit dès l’ouverture ; aucune réapparition
  au prochain lancement, même si l’utilisateur n’a rien choisi.
- Si le fournisseur immédiat n’est pas disponible (pas de distributor, pas de
  Play Services), l’option est grisée avec une explication ; on ne repose pas la
  question plus tard.
- Un mode explicitement choisi plus tard dans Réglages s’applique immédiatement ;
  l’absence de choix n’est jamais traitée comme « Différé » silencieusement.

### Ensuite

Le même choix est toujours accessible dans **Réglages → Notifications et
arrière-plan** :

- mode ;
- intervalle 15/30/60 min ;
- fournisseur immédiat (Auto / Sans Google / Google) ;
- état réel : dernier réveil, dernière synchro, données du mois ;
- bouton « Tester » ;
- bouton « Modifier le fournisseur ».

Aucun autre écran ne redemande le mode. Une mise à jour de l’app ne le repropose
pas : seul `notif.onboardingV1` absent déclenche la feuille. Tant que
`notif.mode=unset`, Réglages affiche « Choisir un mode » et aucun job n’est
programmé.

## 11. État implémenté (Colo Course, 2026-11-10)

- `notif.mode` = `unset|off|deferred` (le mode `immediate` est réservé et
  actuellement désactivé dans l’UI).
- Feuille one-shot sans présélection ; « Plus tard » laisse `unset` et ne
  programme rien.
- Mode différé : `JobScheduler` 15/30/60 min, `setPersisted(true)`, contrainte
  réseau + batterie pas faible, aucune notification permanente.
- L’ancien `PushService` 5 s n’est plus démarré par l’app ; il est arrêté dès
  qu’un mode est appliqué.
- Les jobs sont annulés quand l’app revient au premier plan (le WebSocket Qt
  reprend la main).
- L’option « Immédiat » est grisée tant que UnifiedPush/FCM ne sont pas livrés.
- Colo Tâches : à porter dans un second temps (pas de service Android actuel).

**Pas encore fait** : vrai worker de sync courte (P2), UnifiedPush (P3),
FCM (P4). Pour l’instant le job différé **notifie** ; la synchronisation des
données se fait à l’ouverture de l’app.

## 12. Spécification P2 — sync courte en arrière-plan

Objectif : que le job différé ne se contente pas de notifier, mais merge les
données manquantes, puis s’arrête.

### Déroulé

1. `JobScheduler` démarre `BackgroundSyncJob` (même process que l’app si elle
   est vivante, sinon process court).
2. Le job ouvre la base SQLite en WAL et prend un verrou court.
3. Il crée un `SyncEngine` minimal (relais déjà configuré, pas de P2P) et
   demande `catchUp` pour les listes dues.
4. Il merge les deltas/snapshots, met à jour l’outbox et les ack.
5. Il affiche au plus une notification locale par liste modifiée.
6. Il ferme la base, libère le verrou, appelle `jobFinished`.
7. Budget maximal : **20 s** de réseau, **5 s** de merge, puis arrêt.

### Garde-fous

- Aucun `PeerConnection`, aucun TURN, aucun signaling.
- Pas de service foreground, pas de notification permanente.
- Si Qt ne peut pas démarrer dans le process du job : repli notification-only.
- Si le merge échoue : ne pas acquitter, garder l’outbox, réessayer au prochain
  job.
- Si l’app est au premier plan : le job est annulé, le WebSocket Qt fait le
  travail.
- Ne jamais lancer deux syncs simultanées : verrou `sync.lock` en base.

### Tests

- Doze + reboot : job reprogrammé, merge OK.
- 20 listes, 1 000 deltas en attente : merge < 5 s, une notification groupée.
- App tuée pendant le merge : transaction SQLite annulée, aucun ack partiel.
- Batterie : `< 1 %/24 h` écran éteint, `< 15 Mo/mois`.

## 13. Spécification P3 — UnifiedPush sans Google

### Client

- Ajouter une abstraction `PushProvider` : `none`, `unifiedpush`, `fcm`.
- Enregistrer l’app auprès du distributor UnifiedPush installé.
- Stocker l’endpoint et le topic localement ; ne jamais envoyer de contenu.
- Si aucun distributor : masquer l’option, repli Différé.
- Si l’utilisateur choisit Immédiat : demander l’exemption batterie uniquement
  dans ce mode, avec explication.

### Serveur

- `POST /push {app, endpoint}` → envoi data-only via le distributor.
- `/config` expose `push.unifiedpush: true` et l’endpoint public.
- Rate-limit par endpoint ; métriques par app.
- Aucun titre, aucune donnée de liste.

### Tests

- Distributor absent : option grisée, aucun crash.
- Distributor installé : notification < 5 s app fermée, Doze inclus.
- Self-host : ntfy comme distributor, pas de Google.

## 14. Spécification P4 — FCM optionnel

- Dépendance Play Services **uniquement** dans le build qui l’active.
- `FirebaseMessagingService` : message data-only vide ; l’app lit le chiffré.
- `google-services.json` n’est pas un secret, mais il est propre à l’instance.
- Si Play Services absent : ne pas proposer FCM, repli UnifiedPush/Différé.
- Serveur : `/push {app, fcmToken}` → FCM ; 202/429 ; jamais de contenu.
- Tests : APK sans Play Services, APK avec, Doze, token renouvelé.

## 15. Portage Colo Tâches

- Mêmes fichiers Java (`DeferredPushJob`, `Platform`), même contrat JNI.
- Mêmes réglages `notif.*` et même feuille one-shot.
- Topics `colo-...` identiques au format, mais app distincte (`colotaches`).
- Le serveur doit garder deux namespaces de métriques.
- Tests : les deux apps sur le même téléphone, pas de collision de topics.

## 16. Contrats serveur

```http
POST /push
{ "app": "colocourse|colotaches", "topic": "...", "provider": "unifiedpush|fcm" }
→ 202 Accepted | 429 Too Many Requests
```

- Aucun titre, aucun article, aucune clé.
- Le serveur ne stocke que le couple `(app, topic, provider, last_seen)`.
- `/config` :

```json
{
  "notifications": {
    "deferred": true,
    "unifiedpush": true,
    "fcm": false,
    "maxInterval": 60
  }
}
```

## 17. Matrice de tests

| Cas | Attendu |
|---|---|
| Aucun mode choisi | 0 job, 0 requête, app synchronisée à l’ouverture |
| Désactivé | 0 job, 0 notification |
| Différé 15 min | ≤ 96 réveils/jour, merge court, pas de notification permanente |
| Immédiat UnifiedPush | < 5 s, pas de service permanent |
| Immédiat FCM | < 5 s, pas de service permanent |
| Distributor absent | option grisée, repli différé |
| Play Services absent | FCM masqué, repli UnifiedPush/différé |
| Doze | job décalé, pas de crash, pas de perte d’outbox |
| Reboot | job reprogrammé |
| App au premier plan | job annulé, WebSocket actif |
| Deux apps | topics/métriques séparés |
| Self-host sans Google | différé + UnifiedPush OK |

## 18. Décisions figées

- **Aucune présélection** du mode.
- **Aucun job** tant que `notif.mode=unset`.
- **Différé** par défaut *proposé*, jamais imposé.
- **Immédiat** optionnel ; sans Google via UnifiedPush, avec Google via FCM.
- **Pas de service Qt/WebSocket permanent** en arrière-plan.
- **Pas de contenu de liste** dans une notification.
- **Perte acceptée** si plus aucun appareil ne détient une liste.

## 19. Definition of done par phase

- P1 : one-shot sans présélection, Réglages, JobScheduler, plus de 5 s.
- P2 : merge SQLite réel dans le job, outbox/ack corrects, tests Doze/reboot.
- P3 : UnifiedPush de bout en bout, repli propre sans distributor.
- P4 : FCM optionnel, APK sans Play Services fonctionnel.
- P5 : métriques dernier réveil/données, tests OEM, doc utilisateur.

## 20. État actuel

- P1 : **fait** (Course v0.33.0).
- P2 : **implémenté (Course + Tâches)** — broadcast si le process Qt est vivant ;
  sinon `BackgroundSyncService` (`QtService`, process `:qt_service`) ouvre la
  base, merge et s’arrête après 30 s max. À valider sur appareil réel
  (Doze/reboot/OEM).
- P3/P4/P5 : **à faire**.
- Colo Tâches : **à porter**.
