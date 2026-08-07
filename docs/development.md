# Développer et tester Tick Every

Ce document décrit l'implémentation actuelle et la procédure utilisée avec
l'app mobile Pebble et `pebble-tool` en 2026.

## Comportement de référence

Tick Every est une watchapp native C et mono-fonction. Son parcours de réglage
est linéaire : **Timer → Délai → Vibrations → Prêt**. Le runtime du timer est
autonome ; un petit composant PebbleKit JS sert uniquement à choisir la langue
depuis **Configure** dans l'app mobile Pebble.

### Boutons et state machine

| État | Up / Down | Select | Back |
| --- | --- | --- | --- |
| Timer | valeur suivante / précédente | passer à Délai | quitter l'app |
| Délai | valeur suivante / précédente | passer à Vibrations | revenir à Timer |
| Vibrations | Oui / Non | passer à Prêt | revenir à Délai |
| Prêt | — | appui long de 0,7 s : lancer | revenir à Vibrations |
| Attente | — | — | demander confirmation de l'arrêt |
| Actif | — | mettre en pause | demander confirmation de l'arrêt |
| Pause | — | reprendre | demander confirmation de l'arrêt |
| Confirmation | — | confirmer l'arrêt | annuler et revenir à l'état précédent |

Un clic court sur Select dans l'écran Prêt joue seulement le halo visuel. Le
timer démarre uniquement après un appui long de 700 ms, ce qui évite un départ
accidentel.

### Timer répétitif et cycles

Le timer configuré `X` est directement l'intervalle entre deux cycles. Il est
sélectionnable de **1 à 3 600 secondes** sur cette grille :

- pas de 1 s jusqu'à 30 s inclus ;
- pas de 5 s au-dessus de 30 s et jusqu'à 120 s inclus ;
- pas de 15 s au-dessus de 120 s et jusqu'à 3 600 s.

Le temps affiché est le temps actif écoulé et n'avance pas pendant une pause.
Le compteur est `temps_actif / X`. Le timer et les cycles continuent sans fin
jusqu'à la confirmation d'un arrêt manuel.

Le calcul exact repose sur `time_ms()` et sur des `AppTimer` alignés sur la
phase du lancement, plutôt que sur le nombre de callbacks reçus. Si un réveil
arrive en retard, l'app recalcule le temps et le cycle courants, puis émet au
maximum le code haptique du dernier cycle atteint. Elle ne rejoue pas une file
de vibrations obsolètes.

### Délai et vibrations

Les délais disponibles sont **0, 5, 10, 15, 30 et 60 secondes**. Aucun cycle
n'est compté et aucune vibration numérotée n'est émise pendant cette attente.
Au démarrage effectif, `vibes_double_pulse()` annonce le début par deux
mini-vibrations, même si les vibrations numérotées sont désactivées. Le cycle
`n` arrive ensuite exactement à `D + nX`.

Exemple obligatoire : avec `D = 10 s` et `X = 5 s`, le double signal arrive à
10 s, le cycle 1 à 15 s, le cycle 2 à 20 s et le cycle 3 à 25 s.

Le code haptique numérote ensuite chaque cycle :

- cycles 1 à 9 : autant de vibrations courtes ;
- à partir du cycle 10 : une vibration longue par dizaine, suivie d'une vibration
  courte par unité ;
- cycle 12 : une longue puis deux courtes ;
- cycle 55 : cinq longues puis cinq courtes.

Dans l'implémentation, une impulsion courte dure 120 ms, une longue 300 ms et
deux impulsions sont séparées par 50 ms. Ces durées rendent notamment les
cycles 1 et 2 nettement plus perceptibles. Le buffer statique accepte jusqu'à
**63 segments**, donc **32 impulsions** au maximum. Le motif doit aussi finir
avec au moins 20 ms de marge avant le cycle suivant, afin que deux patterns ne
se chevauchent pas et que le buffer ne soit pas réutilisé trop tôt. Si le code
décimal d'un numéro absolu dépasse l'une de ces limites, le motif concerné est
loggé puis sauté. Le temps actif, le numéro de cycle et l'interface continuent
sans interruption.

Le toggle Vibrations coupe uniquement les codes numérotés. Le double pulse de
départ reste toujours émis. Le timer, le délai, le toggle et la langue sont
écrits dans le stockage persistant et relus au prochain lancement. Une valeur
absente ou invalide est remplacée par le défaut : 5 s, aucun délai, vibrations
activées et interface anglaise.

### Configuration mobile de la langue

Le manifeste déclare la capability `configurable`. Depuis la fiche Tick Every
dans l'app mobile, **Configure** ouvre la page HTTPS statique
`src/pkjs/config.html`. Elle propose English et Français, sans analytics,
cookie ou script tiers. `src/pkjs/index.js` valide la réponse puis envoie la
clé AppMessage `LANGUAGE` (`0` pour l'anglais, `1` pour le français).

La montre valide à nouveau cette valeur, l'écrit sous `PERSIST_KEY_LANGUAGE`
et redessine l'état courant. Une valeur absente, mal formée ou hors de 0/1 est
refusée. Le timer continue de fonctionner sans téléphone ni réseau ; Internet
est requis uniquement pour charger la page de configuration.

### Interface et mémoire

L'interface « Signal clair » dessine un fond blanc très contrasté. Un bandeau
arrondi identifie l'état ; pendant la configuration, il porte aussi l'étape
`1/4` à `4/4`. La valeur principale utilise `FONT_KEY_BITHAM_42_BOLD`, ou
`FONT_KEY_BITHAM_30_BLACK` pour les valeurs longues. Les informations de
contexte et les deux lignes d'actions utilisent
`FONT_KEY_GOTHIC_18_BOLD`. Les copies restent courtes et la valeur conserve
toujours la priorité visuelle.

En exécution, le temps actif reste dominant et deux métriques explicites sont
affichées dessous : `CYCLE n` et `TOUTES LES X s`. Les couleurs du bandeau
distinguent la configuration, l'activité, la pause et la confirmation. Sur les
écrans noir et blanc, le bandeau devient noir et le fond reste blanc. Sur les
écrans ronds, la state machine et les boutons ne changent pas. Une UI dédiée
exploite le cercle : anneau extérieur, capsule d'état placée sur une corde
sûre, valeur centrale et deux actions en capsules. Chalk et Gabbro ont des
géométries et tailles de police distinctes ; Gabbro utilise un affichage
numérique 60 px pour le temps et les cycles.

Un halo fin et discret apparaît au démarrage, à la reprise et à chaque cycle.
Les animations utilisent un seul `AppTimer`, avec une frame toutes les 70 ms.
Un second `AppTimer` assure les réveils exacts du runtime. Les marges et tailles
s'adaptent aussi à la définition d'emery.

Pour rester compatible avec le heap utile limité d'aplite — environ 24 Ko,
contre 64 Ko ou plus sur basalt et les plateformes suivantes — l'app utilise
une seule `Window`, un seul `Layer` personnalisé, deux `AppTimer` au maximum et
des buffers statiques. Les timers sont annulés et le layer détruit dans
`window_unload`; les vibrations, le tick service et la fenêtre sont libérés
dans `deinit`.

## Tests automatiques

La logique indépendante du SDK est dans `src/c/timer_logic.c`. Sa suite hôte
couvre la grille du timer et ses seuils de pas, les délais, les exemples de
codes haptiques, les cycles sans clamp final, les réveils tardifs, les phases
exactes en millisecondes et les bornes d'overflow.

```sh
./tests/run.sh
pebble build
```

`tests/run.sh` compile la logique portable en C99 avec
`-Wall -Wextra -Werror -pedantic`, exécute ses 19 146 assertions, puis teste en
Node la validation, la persistance et le retry de la langue côté PebbleKit JS.
`pebble build` compile la watchapp pour toutes les plateformes déclarées et
produit `build/tick-every.pbw`.

## Boucle émulateur

La vérification complète après une modification est :

```sh
./tests/run.sh
pebble build
pebble install --emulator basalt
pebble screenshot --emulator basalt docs/screenshots/capture-basalt.png --no-open
pebble logs --emulator basalt
```

Après chaque modification : corriger toute erreur de test ou de build,
installer, parcourir l'interface, prendre une capture, vérifier visuellement le
rendu, puis lire les logs. Les captures de référence sont conservées dans
[`docs/screenshots/`](screenshots/).

Tester au minimum ces scénarios :

1. régler `X = 5 s`, sans délai, vérifier les cycles 1, 2 et 3 à 5, 10 et 15 s,
   puis vérifier que le timer reste actif ;
2. régler `D = 10 s` et `X = 5 s` : vérifier l'attente silencieuse, le double
   signal à 10 s, puis les cycles 1, 2 et 3 à 15, 20 et 25 s ;
3. désactiver Vibrations, vérifier l'absence de code numéroté et la présence du
   double pulse de départ ;
4. mettre en pause, attendre, reprendre et vérifier que le temps en pause
   n'est pas ajouté au temps écoulé ;
5. ouvrir la confirmation d'arrêt depuis Attente, Actif et Pause, puis tester
   l'annulation et la confirmation ;
6. confirmer l'arrêt, vérifier le retour à Prêt avec temps et cycle à zéro,
   puis relancer par un appui long sur Select ;
7. vérifier les codes des cycles 1, 9, 12 et 55, puis vérifier qu'un motif hors
   limite est sauté sans arrêter le compteur ni l'interface ;
8. relancer l'app et vérifier la persistance de l'intervalle, du délai, du
   toggle haptique et de la langue ;
9. depuis **Configure** dans l'app mobile, tester English → Français → English,
   vérifier le changement immédiat et la persistance après relance ;
10. contrôler le rendu sur basalt, chalk et aplite au minimum.

Lors des vibrations, l'émulateur peut écrire des warnings de la forme
`PHONESIM ... QemuInboundPacket.footer`. Ils viennent de la simulation QEMU et
ne correspondent pas à un `APP_LOG(APP_LOG_LEVEL_ERROR, ...)` de Tick Every.

## Émulateurs

`pebble-tool 5.0.39` avec le SDK 4.17 expose les plateformes suivantes :

| Identifiant | Produit simulé | Écran |
| --- | --- | --- |
| `aplite` | Pebble Classic, Pebble Steel | 144×168, rectangulaire, noir et blanc |
| `basalt` | Pebble Time, Pebble Time Steel | 144×168, rectangulaire, 64 couleurs |
| `chalk` | Pebble Time Round | 180×180, rond, 64 couleurs |
| `diorite` | Pebble 2 | 144×168, rectangulaire, noir et blanc |
| `flint` | Pebble 2 Duo | 144×168, rectangulaire, noir et blanc |
| `emery` | Pebble Time 2 | 200×228, rectangulaire, 64 couleurs |
| `gabbro` | Pebble Round 2 | 260×260, rond, 64 couleurs |

Une app ne peut être installée que si son `targetPlatforms` contient la
plateforme choisie. Tick Every cible les sept plateformes : aplite, basalt,
chalk, diorite, emery, flint et gabbro.

### Piloter les boutons

On peut cliquer les boutons dessinés autour de l'émulateur ou utiliser le
clavier :

| Bouton | Clavier |
| --- | --- |
| Back | `Q` ou flèche gauche |
| Up | `W` ou flèche haut |
| Select | `S` ou flèche droite |
| Down | `X` ou flèche bas |

Les mêmes actions sont pilotables en CLI :

```sh
pebble emu-button click up --emulator basalt
pebble emu-button click select --emulator basalt
pebble emu-button click down --emulator basalt
pebble emu-button click back --emulator basalt
```

Maintenir puis relâcher Select permet de tester l'appui long :

```sh
pebble emu-button push select --emulator basalt
# attendre au moins 0,7 s
pebble emu-button release select --emulator basalt
```

Répéter cinq clics :

```sh
pebble emu-button click up --repeat 5 --emulator basalt
```

### État simulé et maintenance

```sh
pebble emu-tap --emulator basalt
pebble emu-battery --percent 20 --emulator basalt
pebble emu-bt-connection --connected no --emulator basalt
pebble emu-bt-connection --connected yes --emulator basalt
```

Si l'installation arrive sur le launcher sans ouvrir l'app :

```sh
pebble emu-button click select --emulator basalt
```

Fermer ou remettre à zéro les émulateurs :

```sh
pebble kill
pebble wipe
```

`pebble wipe` efface aussi le stockage persistant de l'app dans l'émulateur.

## Installation sur une montre — workflow 2026

Le workflow recommandé passe par Dev Connection et le relais CloudPebble. Le
Mac et le téléphone n'ont pas besoin d'être sur le même réseau.

### 1. Vérifier le compte de l'app mobile

Dans l'app Pebble :

1. ouvrir **Settings** ;
2. ouvrir la section **General** ;
3. lire le compte indiqué sous **Sign Out – Pebble Account**.

Si **Sign In – Pebble Account** est affiché à la place, aucun compte n'est
connecté. L'app 2026 accepte Google, Apple et GitHub ; GitHub n'est pas
obligatoire.

### 2. Activer Dev Connection

1. ouvrir l'écran qui liste les montres ;
2. ouvrir le menu `⋯` de la montre connectée ;
3. activer **Dev Connection** ;
4. vérifier que **Connected to CloudPebble** apparaît.

Si **Dev Connection** est grisé, se connecter d'abord à un Pebble Account.

### 3. Vérifier le compte de la CLI

```sh
pebble login --status
```

La CLI et l'app mobile doivent utiliser le même Pebble Account. Si nécessaire :

```sh
pebble logout
pebble login
```

`pebble login` ouvre l'authentification dans le navigateur.

### 4. Installer et lire les logs

```sh
pebble build
pebble install --cloudpebble
pebble logs --cloudpebble
```

Ne pas reprendre l'ancien workflow **Developer Mode** sans vérifier l'app
mobile actuelle.

## Alternative LAN

Le cloud est préférable. Pour utiliser le réseau local :

1. dans les settings de l'app, activer **Use LAN developer connection** ;
2. dans le menu `⋯` de la montre, activer **Dev Connection** ;
3. relever l'adresse IPv4 affichée ;
4. garder le Mac et le téléphone sur le même réseau de confiance ;
5. utiliser cette adresse :

```sh
pebble install --phone ADRESSE_IPV4
pebble logs --phone ADRESSE_IPV4
```

Le mode LAN n'est pas chiffré.

## Références

- [Installer le SDK](https://developer.repebble.com/sdk/)
- [Hardware Information](https://developer.repebble.com/guides/tools-and-resources/hardware-information/)
- [App mobile Pebble actuelle](https://github.com/coredevices/mobileapp)
- [Pebble Tool](https://github.com/coredevices/pebble-tool)
