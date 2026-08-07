# Tick Every

<p align="center">
  <img src="store/assets/icon-144.png" width="112" height="112" alt="Tick Every icon">
</p>

<p align="center">
  <strong>An endless repeating timer that tells you the cycle number through vibrations.</strong><br>
  Native, private and built for Pebble watches.
</p>

<p align="center">
  <a href="#english">English</a> · <a href="#français">Français</a>
</p>

---

## English

Tick Every repeats one interval until you stop it. At the end of each cycle,
the watch uses a numbered haptic pattern so you can keep count without looking
at the screen. An optional start delay gives you time to get ready.

There is no Tick Every account, analytics or advertising. The timer itself
runs entirely on the watch and does not need a phone or Internet connection.
English is the default interface language; French can be selected from
**Configure** in the Pebble mobile app. Opening that hosted settings page
requires Internet access, but the saved choice and every timer feature work
offline afterwards.

### How it works

Watch setup is a four-step flow: **Timer → Delay → Haptics → Ready**. Set an
**interval**, an optional **start delay**, and whether numbered haptics are
enabled. Hold **Select** for 0.7 seconds to start.

To change the watch language, open Tick Every in the Pebble mobile app, choose
**Configure**, select English or Français, then save. The phone sends the
choice to the watch, where it is persisted.

For a 10-second delay and a 5-second interval:

| Time | What happens |
| ---: | --- |
| 0–10 s | Silent start delay |
| 10 s | Two short pulses announce the actual start |
| 15 s | Cycle 1 — one short vibration |
| 20 s | Cycle 2 — two short vibrations |
| 25 s | Cycle 3 — three short vibrations |
| … | The timer continues until you stop it |

Cycles 1–9 use the same number of short vibrations. From cycle 10 onward, long
vibrations represent tens and short vibrations represent units: cycle 12 is
**one long + two short**, and cycle 55 is **five long + five short**.

Numbered haptics can be disabled. The two short start pulses remain enabled so
you still know when the delay has ended.

### Controls and features

- **Up / Down:** change the current watch setting.
- **Select:** confirm a setting; pause or resume a running timer.
- **Hold Select:** start from the Ready screen.
- **Back:** return to the previous setting, or open the stop confirmation while
  the timer is running.
- Repeating intervals from **1 second to 1 hour**.
- Optional delays of **0, 5, 10, 15, 30 or 60 seconds**.
- Phase-accurate elapsed time and cycle count, including after a late wake-up.
- Persistent interval, delay, haptic and language settings.
- High-contrast layouts for rectangular, round, colour and monochrome screens.
- English and French interface; English by default.

### Screenshots

| Configure the interval | Follow the current cycle | Round display |
| :---: | :---: | :---: |
| <img src="docs/screenshots/01-timer-basalt.png" width="144" alt="Interval setup on a rectangular colour Pebble"> | <img src="docs/screenshots/10-running-cycle3-basalt.png" width="144" alt="Running timer at cycle 3"> | <img src="docs/screenshots/13-timer-chalk.png" width="180" alt="Interval setup on a Pebble Time Round"> |

More states and platform variants are available in
[`docs/screenshots/`](docs/screenshots/). The illustrated user guide is
[`docs/tutorial.html`](docs/tutorial.html).

### Install a PBW

The simplest installation method is the Pebble Appstore. To sideload a build,
download `tick-every.pbw` from the latest
[GitHub release](https://github.com/G-Cyrille/tick-every/releases/latest), then
install it with a current Pebble SDK and `pebble-tool`:

```sh
pebble login --status
pebble install --cloudpebble /path/to/tick-every.pbw
```

The Pebble mobile app and the CLI must use the same Pebble Account, and **Dev
Connection** must be connected to CloudPebble. See the
[watch installation guide](docs/development.md#installation-sur-une-montre--workflow-2026)
for the complete workflow and the LAN fallback.

### Build and test

Prerequisites:

- a current Pebble SDK;
- `pebble-tool`;
- a C99 compiler for the host-side tests.

```sh
git clone https://github.com/G-Cyrille/tick-every.git
cd tick-every
./tests/run.sh
pebble build
pebble install --emulator basalt
```

`./tests/run.sh` compiles the portable timer logic with strict compiler flags,
runs **19,146 C assertions**, then checks the mobile language validation,
persistence and retry flow in Node. `pebble build` builds every platform declared
in `package.json` and creates `build/tick-every.pbw`.

For UI work, inspect at least one rectangular colour target (`basalt`), one
round target (`chalk` or `gabbro`) and one monochrome target (`aplite` or
`diorite`):

```sh
pebble install --emulator chalk
pebble screenshot --emulator chalk screenshot-chalk.png --no-open
pebble logs --emulator chalk
```

The detailed emulator, hardware and regression-test workflow is documented in
[`docs/development.md`](docs/development.md).

### Architecture

Tick Every's runtime is a native C watchapp. A small PebbleKit JS component
opens the mobile language page and sends the saved choice to the watch through
AppMessage; it is not needed while a timer is running.

```text
src/c/tick-every.c   UI, state machine, persistence, scheduling and haptics
src/c/timer_logic.*  Portable interval, delay, cycle and haptic-code logic
src/pkjs/index.js    Mobile configuration lifecycle and AppMessage delivery
src/pkjs/config.html Hosted English/French language form
tests/               Host-side C tests for timer_logic
resources/images/    Pebble launcher icon
docs/                Development guide, tutorial and reference screenshots
store/               Appstore copy, artwork, screenshots and release material
```

The runtime derives elapsed time and cycle boundaries from `time_ms()` instead
of counting callbacks. If a callback is late, the display catches up to the
current cycle without replaying a queue of stale vibration patterns. Static
buffers and a small number of layers and timers keep the app compatible with
the tighter heap on Aplite.

Supported platforms: `aplite`, `basalt`, `chalk`, `diorite`, `emery`, `flint`
and `gabbro`.

### Contribute

Bug reports, feature ideas, translations and tested pull requests are welcome.

1. Search the existing issues, then use the appropriate
   [issue template](https://github.com/G-Cyrille/tick-every/issues/new/choose).
2. Fork the repository and create a focused branch.
3. Keep the platform constraints in mind and add tests for logic changes.
4. Run `./tests/run.sh` and `pebble build`.
5. For UI changes, attach before/after screenshots for rectangular, round and
   monochrome targets.

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full contribution workflow.

---

## Français

Tick Every est un timer répétitif sans fin. À chaque cycle, la montre encode le
numéro du cycle avec des vibrations : les vibrations courtes représentent les
unités et, à partir de 10, les vibrations longues représentent les dizaines.
On peut donc suivre sa progression sans regarder l'écran.

Exemple avec un délai de 10 s et un intervalle de 5 s : double mini-vibration à
10 s, cycle 1 à 15 s, cycle 2 à 20 s, cycle 3 à 25 s, puis le timer continue
jusqu'à son arrêt manuel.

Le timer fonctionne sur la montre, sans compte Tick Every, analytics ou
publicité. Il n'a besoin ni du téléphone ni d'Internet pendant son utilisation.
L'anglais est la langue par défaut. Pour passer en français, ouvrir Tick Every
dans l'app mobile Pebble, choisir **Configure**, sélectionner Français puis
enregistrer. L'ouverture de cette page hébergée nécessite Internet ; le choix
est ensuite sauvegardé et le timer fonctionne hors ligne.

### Utiliser l'app

Le réglage sur la montre suit un parcours linéaire : **Timer → Délai →
Vibrations → Prêt**.

- **Up / Down** modifient la valeur affichée.
- **Select** valide ; pendant le timer, il met en pause ou reprend.
- Un appui long de **0,7 s sur Select** lance le timer depuis l'écran prêt.
- **Back** revient au réglage précédent ou demande confirmation avant l'arrêt.
- L'intervalle va de **1 seconde à 1 heure**.
- Le délai peut être **0, 5, 10, 15, 30 ou 60 secondes**.
- Les vibrations numérotées peuvent être coupées ; le double signal de départ
  reste actif.
- Les réglages sont sauvegardés sur la montre.

Pour installer un PBW, développer l'app ou contribuer, suivre les sections
anglaises [Install a PBW](#install-a-pbw), [Build and test](#build-and-test) et
[Contribute](#contribute). Le guide technique détaillé en français est dans
[`docs/development.md`](docs/development.md).

## Project links

- [Appstore publication material](store/submission-checklist.md)
- [Publication runbook](store/PUBLISHING.md)
- [Illustrated user guide](docs/tutorial.html)
- [Changelog](CHANGELOG.md)
- [Contributing guide](CONTRIBUTING.md)
- [Official Pebble developer documentation](https://developer.repebble.com/)

Maintained by **G-Cyrille**.
