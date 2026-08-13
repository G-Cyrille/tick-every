# Publishing Tick Every

This runbook prepares and publishes Tick Every 1.2.2 to the Pebble Appstore.
Run it only from the repository root after the release commit and GitHub URL
exist.

Versions up to 1.2.1 were published as Appstore ID
`f64d58f70cb8458390cd7749`. The public listing is
<https://apps.repebble.com/f64d58f70cb8458390cd7749>.

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
- `store/release/tick-every-v1.2.2.pbw` matches `build/tick-every.pbw`.
- `store/SHA256SUMS` matches the release PBW.
- The Pebble CLI is logged into the same Pebble Account as the mobile app.
- Every completed item in `store/submission-checklist.md` has been checked.

## Final verification

```sh
./tests/run.sh
pebble build
shasum -a 256 store/release/tick-every-v1.2.2.pbw
pebble login --status
```

The expected SHA-256 for this build is:

```text
7640a99059c2edded6ecf1fbbffd3637ffe9f772106b3beb281545ccbef58442
```

Open and inspect at least these files before publishing:

- `store/description.txt`;
- `store/release-notes.md`;
- `store/assets/icon-80.png` and `store/assets/icon-144.png`;
- every PNG in `store/screenshots/`;
- `store/privacy-policy.html`;
- `docs/tutorial.html`.

## Publish from the CLI

```sh
TICK_EVERY_DESCRIPTION="$(<store/description.txt)"
TICK_EVERY_RELEASE_NOTES="$(<store/release-notes.md)"

pebble publish \
  --non-interactive \
  --no-gif-all-platforms \
  --name "Tick Every" \
  --version "1.2.2" \
  --description "$TICK_EVERY_DESCRIPTION" \
  --release-notes "$TICK_EVERY_RELEASE_NOTES" \
  --source "https://github.com/G-Cyrille/tick-every" \
  --category tools \
  --icon-small store/assets/icon-80.png \
  --icon-large store/assets/icon-144.png
```

The tool rebuilds the PBW before uploading it. Do not modify source or assets
between the final verification and this command.

For an existing app, `--screenshots` adds the supplied files to the current
listing. The command above sends no screenshot, so the listing keeps its
existing images without creating duplicates. Updated history screenshots stay
in the repository as references. When screenshots need to be added again, use
`--screenshots` as the last argument: it consumes every following path.

## Post-publication checks

1. Open the Appstore listing and verify the name, developer, description,
   icons, screenshots, source URL and version.
2. Install the Appstore build on a physical watch.
3. Open **Configure** in the Pebble mobile app and switch English → Français →
   English.
4. Run the 10-second delay / 5-second interval reference scenario.
5. Confirm the start signal at 10 seconds and cycles at 15, 20 and 25 seconds.
6. Verify pause, resume and stop confirmation.
7. Enable session saving in **Configure**, stop a completed session, then check
   that it appears both in mobile Configure and in the watch history opened by
   holding **Select** on the Timer screen.
8. With a 10-second delay and 60 seconds of active ticks, verify that both
   histories show `1:00`, not `1:10`.
9. Verify that the phone archive grows beyond 32 sessions while the watch keeps
   only its 32 most recent entries.
10. Check cycles 4, 5, 7 and 12 on a physical watch: one long pulse represents
   five cycles and adjacent short pulses remain distinct.

If a release-blocking problem appears, stop promoting the listing and use the
Pebble developer dashboard to correct its publication state. Prepare a tested
patch release rather than replacing the UUID.
