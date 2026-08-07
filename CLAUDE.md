# Tick Every — instructions de développement

## Commandes essentielles

- Build : `pebble build`
- Lancer l'émulateur : `pebble install --emulator basalt` (remplacer `basalt` par la plateforme voulue)
- Capture d'écran : `pebble screenshot --emulator basalt`
- Logs : `pebble logs --emulator basalt`
- Nettoyage : `pebble clean`

## Boucle de développement

Après chaque modification de code :

1. lancer `pebble build` ;
2. en cas d'erreur, lire le diagnostic, corriger, puis relancer le build ;
3. installer l'app avec `pebble install --emulator basalt` ;
4. prendre une capture avec `pebble screenshot --emulator basalt` ;
5. vérifier visuellement le rendu ;
6. consulter les logs avec `pebble logs --emulator basalt`.

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
