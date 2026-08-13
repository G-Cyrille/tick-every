# Release notes

## 1.2.2 — English

History now shows the active tick duration instead of including the start
delay and pauses. The watch displays date and duration first, then delay and
tick count. Existing sessions are corrected automatically.

## 1.2.2 — Français

L'historique affiche maintenant la durée active des ticks sans inclure le
délai de départ ni les pauses. La montre affiche d'abord la date et la durée,
puis le délai et le nombre de ticks. Les sessions existantes sont corrigées
automatiquement.

## 1.2.1 — English

Fixes phone history synchronization in the Pebble mobile app: session
snapshots sent by the watch were ignored, so the Configure archive stayed
empty. The watch keeps its 32 latest sessions and resynchronizes them
automatically after updating.

## 1.2.1 — Français

Corrige la synchronisation de l'historique dans l'app mobile Pebble : les
snapshots envoyés par la montre étaient ignorés et l'archive de Configure
restait vide. La montre conserve ses 32 dernières sessions et les
resynchronise automatiquement après la mise à jour.

## 1.2.0 — English

The watch still keeps 32 recent sessions, while the phone now archives every
session it successfully receives without an app-imposed count limit. Configure
shows the archive in pages of 32. Long vibrations now represent groups of five
cycles, and the quiet gap between pulses is doubled for clearer counting.

## 1.2.0 — Français

La montre conserve toujours 32 sessions récentes, tandis que le téléphone
archive toutes celles qu'il reçoit sans plafond imposé par l'app. Configure les
affiche par pages de 32. Les vibrations longues représentent maintenant des
groupes de cinq cycles et le silence entre impulsions est doublé.

## 1.1.0 — English

Optional local history for the 32 latest completed sessions, available on the
watch and in mobile Configure. Saving is off by default. This release also adds
crash-safe history persistence and more reliable settings synchronization.

## 1.1.0 — Français

Historique local optionnel des 32 dernières sessions terminées, consultable sur
la montre et dans « Configure ». La sauvegarde est désactivée par défaut. Cette
version améliore aussi la résistance aux coupures et la synchronisation des
réglages.

## 1.0.0 — English

Initial release of Tick Every: an indefinite repeating timer with an optional start delay, clear cycle display, pause/resume controls, configurable haptic cycle counting, English by default and French through the mobile configuration page.

## 1.0.0 — Français

Première version de Tick Every : timer répétitif sans fin, délai de démarrage optionnel, affichage clair des cycles, pause/reprise, comptage haptique configurable, anglais par défaut et français via la page de configuration mobile.
