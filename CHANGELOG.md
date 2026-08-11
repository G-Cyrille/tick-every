# Changelog

Toutes les modifications notables de Tick Every sont documentées ici. Le projet suit [Semantic Versioning](https://semver.org/).

## [1.2.0] — 2026-08-11

### Added

- Archive mobile locale sans plafond de sessions imposé par l'app, paginée par
  groupes de 32 dans Configure.

### Changed

- La montre reste limitée aux 32 sessions récentes ; chaque snapshot est
  fusionné dans l'archive du téléphone au lieu de la remplacer.
- Une vibration longue représente maintenant cinq cycles et les vibrations
  courtes le reste : le cycle 12 produit deux longues puis deux courtes.
- Le silence entre deux impulsions passe de 50 à 100 ms pour rendre les suites
  de trois ou quatre vibrations nettement séparées.

### Fixed

- Un historique vide après reset de la montre n'efface plus l'archive mobile.
- Les erreurs de quota `localStorage` conservent l'archive et le curseur de
  synchronisation précédents et affichent un avertissement dans Configure.
- La pagination de l'historique conserve les changements de langue et de
  statistiques tant que l'utilisateur n'a pas encore appuyé sur Save.

## [1.1.0] — 2026-08-11

### Added

- Sauvegarde optionnelle des 32 dernières sessions, désactivée par défaut.
- Historique consultable sur la montre par appui long sur Select depuis le
  premier écran.
- Historique consultable dans la page de configuration mobile.
- Durée totale, durée active, cycles, intervalle, délai, état haptique et date
  de fin pour chaque session.

### Changed

- Synchronisation locale montre → PebbleKit JS avec validation de schéma et
  CRC-32.
- Persistance crash-safe par double banque A/B sur la montre.
- Retries bornés pour les réglages mobiles et coalescing des snapshots
  d'historique quand l'Outbox est occupé.
- Documentation et privacy policy mises à jour pour décrire le stockage local.

## [1.0.0] — 2026-08-08

### Added

- Timer répétitif qui continue jusqu’à l’arrêt manuel.
- Intervalle configurable et délai de démarrage optionnel.
- Double vibration courte au démarrage effectif.
- Comptage haptique des cycles, avec dizaines en vibrations longues et unités en vibrations courtes.
- Activation ou désactivation du comptage haptique.
- Pause, reprise et confirmation d’arrêt.
- Interface adaptée aux écrans rectangulaires, ronds, couleur et noir et blanc.
- Persistance locale des réglages.
