# Développer et tester Tick Every

Cette procédure correspond à l'app mobile Pebble et à `pebble-tool` utilisés
en 2026.

## Boucle locale

```sh
pebble build
pebble install --emulator basalt
pebble screenshot --emulator basalt screenshot_basalt.png --no-open
pebble logs --emulator basalt
```

Après une modification : build, correction des erreurs, installation, capture
d'écran, vérification visuelle, puis lecture des logs.

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
plateforme choisie. Tick Every cible actuellement aplite, basalt, chalk,
diorite et emery.

### Boutons

On peut cliquer les boutons dessinés autour de l'émulateur ou utiliser le
clavier :

| Bouton | Clavier | Usage habituel |
| --- | --- | --- |
| Back | `Q` ou flèche gauche | Revenir ou quitter |
| Up | `W` ou flèche haut | Monter |
| Select | `S` ou flèche droite | Valider |
| Down | `X` ou flèche bas | Descendre |

Les mêmes actions sont pilotables en CLI :

```sh
pebble emu-button click up --emulator basalt
pebble emu-button click select --emulator basalt
pebble emu-button click down --emulator basalt
pebble emu-button click back --emulator basalt
```

Maintenir un bouton et le relâcher :

```sh
pebble emu-button push up --emulator basalt
pebble emu-button release up --emulator basalt
```

Répéter cinq clics :

```sh
pebble emu-button click up --repeat 5 --emulator basalt
```

### État simulé

```sh
pebble emu-tap --emulator basalt
pebble emu-battery --percent 20 --emulator basalt
pebble emu-bt-connection --connected no --emulator basalt
pebble emu-bt-connection --connected yes --emulator basalt
```

Fermer ou remettre à zéro les émulateurs :

```sh
pebble kill
pebble wipe
```

Si l'installation arrive sur le launcher sans ouvrir l'app :

```sh
pebble emu-button click select --emulator basalt
```

## Installation sur une montre — workflow 2026

Le workflow recommandé passe par Dev Connection et le relais CloudPebble. Le
Mac et le téléphone n'ont pas besoin d'être sur le même réseau.

### 1. Vérifier le compte de l'app mobile

Dans l'app Pebble :

1. ouvrir **Settings** ;
2. ouvrir la section **General** ;
3. lire le compte indiqué sous **Sign Out – Pebble Account**.

Si **Sign In – Pebble Account** est affiché à la place, aucun compte n'est
connecté. L'app 2026 accepte Google, Apple et GitHub.

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

## Alternative LAN

Le cloud est préférable. Pour utiliser le réseau local :

1. dans les settings de l'app, activer **Use LAN developer connection** ;
2. dans le menu `⋯` de la montre, activer **Dev Connection** ;
3. relever l'adresse IPv4 affichée ;
4. garder le Mac et le téléphone sur le même réseau ;
5. utiliser cette adresse :

```sh
pebble install --phone ADRESSE_IPV4
pebble logs --phone ADRESSE_IPV4
```

Le mode LAN n'est pas chiffré et ne doit être utilisé que sur un réseau de
confiance.

## Références

- [Installer le SDK](https://developer.repebble.com/sdk/)
- [Hardware Information](https://developer.repebble.com/guides/tools-and-resources/hardware-information/)
- [App mobile Pebble actuelle](https://github.com/coredevices/mobileapp)
- [Pebble Tool](https://github.com/coredevices/pebble-tool)

