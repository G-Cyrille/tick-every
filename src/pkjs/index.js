var messageKeys = require('message_keys');

var CONFIG_URL = 'https://s3.grossholtz.net/public/tick-every/config/2026/08/ea2e99d6-723f-468a-90a2-a1d2a8a5400a-config.html';
var STORAGE_KEY_LANGUAGE = 'tick-every-language';
var LANGUAGE_ENGLISH = 0;
var LANGUAGE_FRENCH = 1;

/* Returns the last mobile choice, repairing missing or invalid values to EN. */
function getStoredLanguage() {
  var stored = localStorage.getItem(STORAGE_KEY_LANGUAGE);
  return stored === String(LANGUAGE_FRENCH) ? LANGUAGE_FRENCH : LANGUAGE_ENGLISH;
}

/* Sends a validated language and reports delivery without changing timer state. */
function sendLanguage(language) {
  var payload = {};
  payload[messageKeys.LANGUAGE] = language;
  Pebble.sendAppMessage(payload, function() {
    console.log('Language sent to watch: ' + language);
  }, function(error) {
    console.log('Language delivery failed; will retry on next app start: ' +
      JSON.stringify(error));
  });
}

/* Opens the hosted mobile form with the current language preselected. */
Pebble.addEventListener('showConfiguration', function() {
  var separator = CONFIG_URL.indexOf('?') === -1 ? '?' : '&';
  Pebble.openURL(CONFIG_URL + separator + 'language=' + getStoredLanguage());
});

/* Validates the form response before updating both phone and watch. */
Pebble.addEventListener('webviewclosed', function(event) {
  if (!event.response) {
    return;
  }

  var configuration;
  try {
    configuration = JSON.parse(decodeURIComponent(event.response));
  } catch (error) {
    console.log('Invalid configuration response: ' + error.message);
    return;
  }

  if (!configuration || typeof configuration.language !== 'number' ||
      (configuration.language !== LANGUAGE_ENGLISH &&
       configuration.language !== LANGUAGE_FRENCH)) {
    console.log('Rejected language value or type');
    return;
  }

  var language = configuration.language;
  localStorage.setItem(STORAGE_KEY_LANGUAGE, String(language));
  sendLanguage(language);
});

Pebble.addEventListener('ready', function() {
  console.log('Tick Every configuration ready');
  sendLanguage(getStoredLanguage());
});
