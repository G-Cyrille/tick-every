# Contributing to Tick Every

Thanks for helping improve Tick Every. Small fixes, platform-specific UI
improvements, translations, tests and well-scoped feature proposals are all
welcome.

[Version française](#français)

## Before opening an issue

Search the [existing issues](https://github.com/G-Cyrille/tick-every/issues)
first. If the problem already exists, add any new reproduction details there
instead of opening a duplicate.

Use the provided issue forms:

- **Bug report:** reproducible incorrect behaviour, crash, unreadable layout or
  platform incompatibility;
- **Feature request:** a new use case or a focused improvement.

For a bug, include the watch model or emulator platform, app version, interface
language, interval, delay, haptic setting, exact steps, expected result and
actual result. Screenshots and relevant `pebble logs` lines are especially
useful for UI and runtime problems. Remove account details and other personal
data before attaching logs.

Security-sensitive reports should not include exploit details or private data
in a public issue. Contact the maintainer directly at `oiawo.cir@gmail.com`.

## Development setup

You need a current Pebble SDK, `pebble-tool`, Git and a C99 compiler.

```sh
git clone https://github.com/G-Cyrille/tick-every.git
cd tick-every
./tests/run.sh
pebble build
pebble install --emulator basalt
```

The complete emulator and hardware workflow is in
[`docs/development.md`](docs/development.md).

## Make a change

1. Fork the repository.
2. Create a branch from the default branch, for example
   `fix/chalk-layout` or `feat/new-delay-option`.
3. Keep the change focused. Avoid unrelated formatting or refactors.
4. Follow the existing C style and explain the intent of non-trivial code.
5. Add or update host-side tests when changing timer, delay, cycle or haptic
   logic.
6. Update the README, development guide, Appstore copy or screenshots when the
   user-visible behaviour changes.
7. Run the verification below before opening a pull request.

Do not change the app UUID. Changing it makes Pebble treat the build as a
different app and breaks upgrades for existing users.

## Verify the change

Every change must pass:

```sh
./tests/run.sh
pebble build
```

For runtime or UI changes:

```sh
pebble install --emulator basalt
pebble screenshot --emulator basalt screenshot-basalt.png --no-open
pebble logs --emulator basalt
```

Test the affected scenario on the relevant targets. UI changes should be
checked on at least:

- `basalt` — 144×168 rectangular colour screen;
- `chalk` or `gabbro` — round screen;
- `aplite` or `diorite` — monochrome screen.

If the change affects sizing or vertical rhythm, also inspect `emery`. Attach
before/after screenshots to the pull request. Do not commit temporary emulator
captures unless they intentionally replace a reference image.

For timer behaviour, a useful baseline is a 10-second start delay and a
5-second interval: start signal at 10 s, then cycles 1, 2 and 3 at 15, 20 and
25 s.

## Pull request checklist

- The issue or use case is explained.
- The change is small enough to review.
- `./tests/run.sh` passes.
- `pebble build` passes for every target in `package.json`.
- New logic has tests.
- UI changes include representative screenshots.
- User-facing behaviour and translations are documented.
- No generated build directory, account data or unrelated files are included.

In the pull request description, include what changed, why, how it was tested
and any platform-specific limitation. A draft pull request is fine when early
feedback would help.

## Technical constraints

- Tick Every's timer runtime is a native C watchapp and must remain independent
  from the phone and network.
- `src/pkjs/index.js` and the hosted `src/pkjs/config.html` page handle only the
  English/French setting. The choice is validated, sent through AppMessage and
  persisted on the watch.
- Aplite has the tightest memory budget; avoid unnecessary layers, timers,
  allocations and large resources.
- Rectangular, round, colour and monochrome layouts must remain usable.
- Timer timing is phase-based. Do not replace it with callback counting.
- Haptic patterns have duration and segment-count limits. A pattern that cannot
  safely finish before the next cycle is skipped while the timer keeps running.
- Allocate and API failures must be checked, and resources must be released in
  the relevant unload or deinitialisation path.

## Français

Les bug reports, petites fonctionnalités, traductions, tests et corrections de
rendu sont bienvenus. Avant de créer une issue, chercher si le sujet existe
déjà, puis utiliser le formulaire adapté.

Pour reproduire un bug, indiquer le modèle de montre ou l'émulateur, la version
de l'app, la langue, l'intervalle, le délai, le réglage haptique, les étapes, le
résultat attendu et le résultat obtenu. Ajouter une capture ou les lignes de
logs utiles si possible, sans donnée personnelle.

Avant une pull request :

```sh
./tests/run.sh
pebble build
```

Une modification d'interface doit aussi être vérifiée sur un écran couleur
rectangulaire, un écran rond et un écran monochrome. Le workflow complet est
dans [`docs/development.md`](docs/development.md).

Ne pas modifier l'UUID de l'app. Garder la pull request ciblée, ajouter les
tests adaptés et expliquer ce qui a été testé. Pour un problème de sécurité,
écrire directement à `oiawo.cir@gmail.com` sans publier de détails sensibles.
