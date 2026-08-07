# Tick Every

App native C pour Pebble. Elle permettra de lancer un tick à intervalle
configurable, avec vibration optionnelle et délai de départ.

## Démarrage rapide

```sh
pebble build
pebble install --emulator basalt
pebble logs --emulator basalt
```

Le build produit `build/tick-every.pbw` pour toutes les plateformes déclarées
dans `package.json`.

## Plateformes cibles

| Identifiant | Montre | Écran |
| --- | --- | --- |
| `aplite` | Pebble Classic, Pebble Steel | 144×168, rectangulaire, noir et blanc |
| `basalt` | Pebble Time, Pebble Time Steel | 144×168, rectangulaire, 64 couleurs |
| `chalk` | Pebble Time Round | 180×180, rond, 64 couleurs |
| `diorite` | Pebble 2 | 144×168, rectangulaire, noir et blanc |
| `emery` | Pebble Time 2 | 200×228, rectangulaire, 64 couleurs |

Le SDK connaît aussi `flint` (Pebble 2 Duo) et `gabbro` (Pebble Round 2), mais
le projet ne les cible pas actuellement.

## Installer sur une montre

Le workflow 2026 utilise Dev Connection via le cloud :

```sh
pebble login --status
pebble build
pebble install --cloudpebble
```

Le téléphone et la CLI doivent utiliser le même Pebble Account. La procédure
détaillée, le mode LAN et les commandes de l'émulateur sont dans
[`docs/development.md`](docs/development.md).

## Structure

```text
src/c/        Code C exécuté sur la montre
src/pkjs/     PebbleKit JS exécuté sur le téléphone
resources/    Images, polices et autres ressources embarquées
package.json  Métadonnées, UUID, plateformes et MessageKeys
wscript       Règles de build Pebble
```

Le projet est une watchapp interactive : `pebble.watchapp.watchface` vaut
`false`.

## Références

- [Documentation de développement](docs/development.md)
- [Documentation officielle Pebble](https://developer.repebble.com/)
