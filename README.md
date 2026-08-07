# Tick Every

Tick Every est une app Pebble native et autonome. Elle lance un timer et
signale chaque cycle par un code haptique qui donne directement son numéro.
Elle n'a ni compte, ni configuration téléphone, ni menu complexe.

## Utilisation

Le réglage suit un parcours linéaire : **Timer → Délai → Vibrations → Prêt**.

- **Up / Down** modifient la valeur affichée ;
- **Select** valide l'étape ;
- **Back** revient à l'étape précédente ;
- sur **Prêt**, maintenir **Select pendant 0,7 s** lance le timer.

Le timer `X` est directement l'intervalle entre deux cycles. Il va de 1 seconde
à 1 heure. Le sélecteur avance sur une grille conçue pour rester rapide :

- pas de 1 s jusqu'à 30 s inclus ;
- pas de 5 s au-dessus de 30 s et jusqu'à 120 s inclus ;
- pas de 15 s au-dessus de 120 s et jusqu'à 3 600 s.

Le délai de départ vaut **0, 5, 10, 15, 30 ou 60 secondes**. Cette attente est
silencieuse. À la fin du délai `D`, la montre émet toujours une double
mini-vibration, puis démarre le temps actif. Le cycle `n` arrive à `D + nX`.
Par exemple, avec `D = 10 s` et `X = 5 s`, le double signal arrive à 10 s,
le cycle 1 à 15 s, le cycle 2 à 20 s et le cycle 3 à 25 s.

Pendant le timer, l'écran affiche le temps écoulé, le numéro du cycle et une
barre d'activité sans fin. **Select** met en pause ou reprend. **Back** ouvre
une confirmation avant l'arrêt. Le timer continue jusqu'à cet arrêt manuel ;
les cycles n'ont pas de limite prédéfinie.

### Code haptique

Quand les vibrations numérotées sont activées :

- les cycles 1 à 9 produisent autant de vibrations courtes ;
- à partir de 10, chaque dizaine du numéro absolu produit une vibration longue,
  puis chaque unité une vibration courte ;
- le cycle 12 produit donc une longue puis deux courtes ;
- le cycle 55 produit cinq longues puis cinq courtes.

Le réglage **Vibrations : Non** coupe uniquement ce code numéroté. Le double
signal qui annonce le démarrage effectif reste actif. Le timer, le délai et ce
réglage sont sauvegardés sur la montre.

Le buffer haptique statique accepte au plus **63 segments**, soit **32
impulsions** séparées par des pauses. Un motif doit aussi finir au moins 20 ms
avant le cycle suivant. Si le numéro absolu du cycle produit un motif trop long
pour l'une de ces limites, ce motif est loggé puis sauté. Le temps, le compteur
de cycles et l'interface continuent normalement ; l'app ne s'arrête pas.

L'interface utilise un halo animé au démarrage et à chaque cycle, une palette
vive sur les écrans couleur et un rendu adapté en noir et blanc. Elle prend en
charge les écrans rectangulaires et ronds des plateformes ciblées.

## Développer

Prérequis : le SDK Pebble et `pebble-tool`.

```sh
./tests/run.sh
pebble build
pebble install --emulator basalt
pebble screenshot --emulator basalt docs/screenshots/capture.png --no-open
pebble logs --emulator basalt
```

Le build produit `build/tick-every.pbw`. Les captures de référence sont dans
[`docs/screenshots/`](docs/screenshots/) et la procédure complète de test et
d'installation est dans [`docs/development.md`](docs/development.md).

## Plateformes cibles

| Identifiant | Montre | Écran |
| --- | --- | --- |
| `aplite` | Pebble Classic, Pebble Steel | 144×168, rectangulaire, noir et blanc |
| `basalt` | Pebble Time, Pebble Time Steel | 144×168, rectangulaire, 64 couleurs |
| `chalk` | Pebble Time Round | 180×180, rond, 64 couleurs |
| `diorite` | Pebble 2 | 144×168, rectangulaire, noir et blanc |
| `emery` | Pebble Time 2 | 200×228, rectangulaire, 64 couleurs |

L'app tient compte du heap réduit d'aplite : une fenêtre, un `Layer` de dessin,
un seul timer d'animation et un buffer haptique statique. Toutes les ressources
sont libérées à la fermeture de la fenêtre ou de l'app.

## Installer sur une montre en 2026

Le workflow recommandé utilise le Pebble Account de l'app mobile et Dev
Connection via CloudPebble :

```sh
pebble login --status
pebble build
pebble install --cloudpebble
```

Le téléphone et la CLI doivent utiliser le même Pebble Account. GitHub n'est
pas obligatoire : l'app mobile accepte aussi Google et Apple. Voir
[`docs/development.md`](docs/development.md#installation-sur-une-montre--workflow-2026)
pour la vérification du compte, les logs et le fallback LAN.

## Structure

```text
src/c/tick-every.c   Interface, state machine, persistance et vibrations
src/c/timer_logic.*  Grille du timer, délai, cycles et code haptique
tests/               Tests C exécutables sur la machine de développement
docs/screenshots/    Captures de référence prises dans les émulateurs
package.json         Métadonnées et plateformes Pebble
wscript              Règles de build
```

Tick Every est une watchapp interactive : `pebble.watchapp.watchface` vaut
`false`.

## Références

- [Documentation de développement](docs/development.md)
- [Documentation officielle Pebble](https://developer.repebble.com/)
