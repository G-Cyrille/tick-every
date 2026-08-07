# Publishing Tick Every

This runbook prepares and publishes Tick Every 1.0.0 to the Pebble Appstore.
Run it only from the repository root after the release commit and GitHub URL
exist.

## Important: publication is immediate

With Pebble Tool 5.0.39, `pebble publish` sends `visible=true` and
`isPublished=true` even when `--is-published` is omitted. It is not a dry-run
or a draft-creation command. Recheck the installed tool's implementation
before a future release because this behaviour may change.

Do not run the final command without the maintainer's explicit approval.

## Prerequisites

- The public repository exists at `https://github.com/G-Cyrille/tick-every`.
- The release commit contains the final source, README, screenshots and
  privacy policy.
- `store/release/tick-every-v1.0.0.pbw` matches `build/tick-every.pbw`.
- `store/SHA256SUMS` matches the release PBW.
- The Pebble CLI is logged into the same Pebble Account as the mobile app.
- Every completed item in `store/submission-checklist.md` has been checked.

## Final verification

```sh
./tests/run.sh
pebble build
shasum -a 256 store/release/tick-every-v1.0.0.pbw
pebble login --status
```

The expected SHA-256 for this build is:

```text
17c6ca9cb49673be67fef6fb7f3ff0c7196a99eb0029e16c69f618f83eb078a7
```

Open and inspect at least these files before publishing:

- `store/description.txt`;
- `store/release-notes.md`;
- `store/assets/icon-80.png` and `store/assets/icon-144.png`;
- every PNG in `store/screenshots/`;
- `store/privacy-policy.html`;
- `docs/tutorial.html`.

## Publish from the CLI

The `--screenshots` argument consumes all following paths, so keep it last.

```sh
TICK_EVERY_DESCRIPTION="$(<store/description.txt)"
TICK_EVERY_RELEASE_NOTES="$(<store/release-notes.md)"

pebble publish \
  --non-interactive \
  --no-gif-all-platforms \
  --name "Tick Every" \
  --version "1.0.0" \
  --description "$TICK_EVERY_DESCRIPTION" \
  --release-notes "$TICK_EVERY_RELEASE_NOTES" \
  --source "https://github.com/G-Cyrille/tick-every" \
  --category tools \
  --icon-small store/assets/icon-80.png \
  --icon-large store/assets/icon-144.png \
  --screenshots \
    store/screenshots/aplite_timer.png \
    store/screenshots/basalt_timer.png \
    store/screenshots/basalt_delay-10s.png \
    store/screenshots/basalt_haptics.png \
    store/screenshots/basalt_ready.png \
    store/screenshots/basalt_start-delay.png \
    store/screenshots/basalt_cycle-3.png \
    store/screenshots/chalk_timer.png \
    store/screenshots/diorite_timer.png \
    store/screenshots/emery_timer.png \
    store/screenshots/flint_timer.png \
    store/screenshots/gabbro_timer.png
```

The tool rebuilds the PBW before uploading it. Do not modify source or assets
between the final verification and this command.

## Post-publication checks

1. Open the Appstore listing and verify the name, developer, description,
   icons, screenshots, source URL and version.
2. Install the Appstore build on a physical watch.
3. Open **Configure** in the Pebble mobile app and switch English → Français →
   English.
4. Run the 10-second delay / 5-second interval reference scenario.
5. Confirm the start signal at 10 seconds and cycles at 15, 20 and 25 seconds.
6. Verify pause, resume and stop confirmation.

If a release-blocking problem appears, stop promoting the listing and use the
Pebble developer dashboard to correct its publication state. Prepare a tested
patch release rather than replacing the UUID.
