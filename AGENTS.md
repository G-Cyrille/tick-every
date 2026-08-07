# Tick Every — instructions de développement

## Commandes essentielles

- Build : `pebble build`
- Lancer l'émulateur : `pebble install --emulator basalt` (remplacer `basalt` par la plateforme voulue)
- Capture d'écran : `pebble screenshot --emulator basalt`
- Logs : `pebble logs --emulator basalt`
- Nettoyage : `pebble clean`

La procédure détaillée est dans `docs/development.md`.

## Boucle de développement

Après chaque modification de code :

1. lancer `pebble build` ;
2. en cas d'erreur, lire le diagnostic, corriger, puis relancer le build ;
3. installer l'app avec `pebble install --emulator basalt` ;
4. prendre une capture avec `pebble screenshot --emulator basalt` ;
5. vérifier visuellement le rendu ;
6. consulter les logs avec `pebble logs --emulator basalt`.

## Émulateur et interactions

- Plateformes du projet : aplite, basalt, chalk, diorite et emery.
- Plateformes supplémentaires connues du SDK : flint et gabbro.
- Clavier QEMU : `Q`/gauche = Back, `W`/haut = Up, `S`/droite = Select, `X`/bas = Down.
- Piloter un bouton en CLI avec `pebble emu-button click select --emulator basalt`.
- Si l'installation reste sur le launcher, envoyer un clic Select.
- Simuler les capteurs et l'état avec `pebble emu-tap`, `pebble emu-battery` et `pebble emu-bt-connection`.

## Installation sur une montre en 2026

- Utiliser le Pebble Account de l'app mobile, via Google, Apple ou GitHub ; GitHub n'est pas obligatoire.
- Dans l'app mobile, le compte actif est visible dans Settings → General → **Sign Out – Pebble Account**.
- Activer **Dev Connection** dans le menu `⋯` de la montre et vérifier **Connected to CloudPebble**.
- Vérifier le compte CLI avec `pebble login --status` : il doit correspondre au compte de l'app mobile.
- Installer avec `pebble install --cloudpebble` et lire les logs avec `pebble logs --cloudpebble`.
- Le fallback LAN nécessite **Use LAN developer connection**, un réseau de confiance et `pebble install --phone ADRESSE_IPV4`.
- Ne pas reprendre l'ancien workflow « Developer Mode » sans vérifier l'app mobile actuelle.

## Contraintes des plateformes

- Heap limité : environ 24 Ko utiles sur aplite, 64 Ko ou plus sur basalt et les plateformes suivantes.
- Écrans : 144×168 rectangulaire, 180×180 rond sur chalk, 200×228 sur emery.
- aplite et diorite sont en noir et blanc ; basalt, chalk et emery prennent en charge la couleur.
- Toujours libérer les ressources dans `window_unload` ou `deinit`.

## Conventions de code

- Commenter l'intention de chaque fonction et expliquer les sections non triviales.
- Logger les points clés avec `APP_LOG(APP_LOG_LEVEL_DEBUG, ...)` : init/deinit, chargement des layers et réception AppMessage.
- Utiliser `APP_LOG_LEVEL_ERROR` pour les échecs d'allocation ou d'API.
- Vérifier tous les retours d'allocation et d'API pouvant échouer.

## Structure du projet

- `src/c/` : code natif de la montre.
- `src/pkjs/` : PebbleKit JS et communication téléphone↔montre.
- `resources/` : images et polices.
