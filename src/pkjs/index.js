var messageKeys = require('message_keys');

var CONFIG_URL = 'https://s3.grossholtz.net/public/tick-every/config/2026/08/fcb3afc5-e97f-4602-bcbe-b8ff756a7508-config.html';
var STORAGE_KEY_LANGUAGE = 'tick-every-language';
var STORAGE_KEY_STATISTICS = 'tick-every-save-statistics';
var STORAGE_KEY_HISTORY = 'tick-every-session-history';
var LANGUAGE_ENGLISH = 0;
var LANGUAGE_FRENCH = 1;
var HISTORY_CAPACITY = 32;
var HISTORY_HEADER_SIZE = 12;
var HISTORY_RECORD_SIZE = 20;
var HISTORY_RECORDS_PER_PAGE = 12;
var HISTORY_PAGE_COUNT = 3;
var SETTINGS_RETRY_COUNT = 3;
var SETTINGS_RETRY_DELAY_MS = 500;
var configurationOpenPending = false;
var configurationOpenTimer = null;
var pendingHistory = null;

/* Returns the last mobile language choice, repairing invalid values to EN. */
function getStoredLanguage() {
  var stored = localStorage.getItem(STORAGE_KEY_LANGUAGE);
  return stored === String(LANGUAGE_FRENCH) ? LANGUAGE_FRENCH : LANGUAGE_ENGLISH;
}

/* Statistics are opt-in, so every missing or invalid value safely means off. */
function getStoredStatisticsEnabled() {
  return localStorage.getItem(STORAGE_KEY_STATISTICS) === '1';
}

/* Validates interval values produced by the watch's three-part timer grid. */
function isSelectableTimer(seconds) {
  if (typeof seconds !== 'number' || seconds % 1 !== 0 ||
      seconds < 1 || seconds > 3600) {
    return false;
  }
  if (seconds <= 30) return true;
  if (seconds <= 120) return (seconds - 30) % 5 === 0;
  return (seconds - 120) % 15 === 0;
}

/* Keeps decoded and locally cached history records on one strict schema. */
function isValidSession(session) {
  var delays = [0, 5, 10, 15, 30, 60];
  return session && typeof session.endedAt === 'number' &&
    session.endedAt > 0 && session.endedAt <= 0xffffffff &&
    typeof session.totalDuration === 'number' &&
    session.totalDuration >= 0 && session.totalDuration <= 0xffffffff &&
    typeof session.activeDuration === 'number' &&
    session.activeDuration >= 0 &&
    session.activeDuration <= session.totalDuration &&
    typeof session.cycles === 'number' && session.cycles >= 0 &&
    session.cycles <= 0xffffffff && isSelectableTimer(session.interval) &&
    delays.indexOf(session.delay) !== -1 &&
    typeof session.haptics === 'boolean';
}

/* Returns only the newest valid records from PebbleKit JS localStorage. */
function getStoredHistory() {
  var parsed;
  var valid = [];
  var index;
  try {
    parsed = JSON.parse(localStorage.getItem(STORAGE_KEY_HISTORY) || '[]');
  } catch (error) {
    parsed = [];
  }
  if (!Array.isArray(parsed)) return valid;
  for (index = 0; index < parsed.length && valid.length < HISTORY_CAPACITY;
       index += 1) {
    if (isValidSession(parsed[index])) valid.push(parsed[index]);
  }
  return valid;
}

/* Reads one little-endian uint32 without introducing signed JS values. */
function readUint32(bytes, offset) {
  return (bytes[offset] | (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) >>> 0;
}

/* Extends the same CRC-32 implementation used by the watch serializer. */
function crc32Update(crc, bytes, start, end) {
  var index;
  var bit;
  for (index = start; index < end; index += 1) {
    crc = (crc ^ bytes[index]) >>> 0;
    for (bit = 0; bit < 8; bit += 1) {
      crc = ((crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0)) >>> 0;
    }
  }
  return crc >>> 0;
}

/* Covers header metadata and records while skipping the stored checksum. */
function historyCrc(bytes) {
  var crc = 0xffffffff;
  crc = crc32Update(crc, bytes, 0, 8);
  crc = crc32Update(crc, bytes, HISTORY_HEADER_SIZE, bytes.length);
  return (~crc) >>> 0;
}

/* Decodes one checksummed page of a newest-first watch snapshot. */
function decodeHistoryPage(bytes) {
  var count;
  var generation;
  var page;
  var start;
  var recordCount;
  var sessions = [];
  var index;
  var offset;
  var flags;
  var session;

  if (!bytes || typeof bytes.length !== 'number' ||
      bytes.length < HISTORY_HEADER_SIZE || bytes[0] !== 84 ||
      bytes[1] !== 69 || bytes[2] !== 72 || bytes[3] !== 49 ||
      bytes[4] !== 1 || bytes[7] >= HISTORY_PAGE_COUNT) {
    return null;
  }
  count = bytes[5];
  generation = bytes[6];
  page = bytes[7];
  start = page * HISTORY_RECORDS_PER_PAGE;
  recordCount = count > start
    ? Math.min(HISTORY_RECORDS_PER_PAGE, count - start) : 0;
  if (count > HISTORY_CAPACITY || (page > 0 && recordCount === 0) ||
      bytes.length !== HISTORY_HEADER_SIZE + recordCount * HISTORY_RECORD_SIZE ||
      readUint32(bytes, 8) !== historyCrc(bytes)) {
    return null;
  }

  for (index = 0; index < recordCount; index += 1) {
    offset = HISTORY_HEADER_SIZE + index * HISTORY_RECORD_SIZE;
    flags = bytes[offset + 19];
    session = {
      endedAt: readUint32(bytes, offset),
      totalDuration: readUint32(bytes, offset + 4),
      activeDuration: readUint32(bytes, offset + 8),
      cycles: readUint32(bytes, offset + 12),
      interval: bytes[offset + 16] | (bytes[offset + 17] << 8),
      delay: bytes[offset + 18],
      haptics: (flags & 1) === 1
    };
    if ((flags & 0xfe) !== 0 || !isValidSession(session)) return null;
    sessions.push(session);
  }
  return {count:count, generation:generation, page:page, sessions:sessions};
}

/* Retries transient delivery failures without blocking the mobile UI forever. */
function sendSettingsAttempt(requestHistory, retriesLeft, success, failure) {
  var payload = {};
  payload[messageKeys.LANGUAGE] = getStoredLanguage();
  payload[messageKeys.SAVE_STATISTICS] =
    getStoredStatisticsEnabled() ? 1 : 0;
  if (requestHistory) payload[messageKeys.HISTORY_REQUEST] = 1;
  Pebble.sendAppMessage(payload, success || function() {
    console.log('Settings sent to watch');
  }, function(error) {
    if (retriesLeft > 0) {
      console.log('Settings delivery failed; retrying: ' +
        JSON.stringify(error));
      setTimeout(function() {
        sendSettingsAttempt(requestHistory, retriesLeft - 1,
                            success, failure);
      }, SETTINGS_RETRY_DELAY_MS);
    } else if (failure) {
      failure(error);
    } else {
      console.log('Settings delivery failed after retries: ' +
        JSON.stringify(error));
    }
  });
}

/* Sends both settings and optionally asks the watch for canonical history. */
function sendSettings(requestHistory, success, failure) {
  sendSettingsAttempt(requestHistory, SETTINGS_RETRY_COUNT, success, failure);
}

/* Compacts one record so a full history remains safe for old webview URLs. */
function compactSession(session) {
  return [session.endedAt, session.totalDuration, session.activeDuration,
    session.cycles, session.interval, session.delay, session.haptics ? 1 : 0];
}

/* Opens the static page with a compact local fragment absent from HTTP logs. */
function openConfiguration() {
  var state;
  var history;
  if (!configurationOpenPending) return;
  configurationOpenPending = false;
  if (configurationOpenTimer !== null) {
    clearTimeout(configurationOpenTimer);
    configurationOpenTimer = null;
  }
  history = getStoredHistory();
  state = {l: getStoredLanguage(), s: getStoredStatisticsEnabled(), h: []};
  history.forEach(function(session) {
    state.h.push(compactSession(session));
  });
  Pebble.openURL(CONFIG_URL + '#state=' +
    encodeURIComponent(JSON.stringify(state)));
}

/* Requests a fresh snapshot but never blocks opening when the watch is absent. */
Pebble.addEventListener('showConfiguration', function() {
  configurationOpenPending = true;
  configurationOpenTimer = setTimeout(openConfiguration, 1500);
  sendSettings(true, function() {}, openConfiguration);
});

/* Validates the form response before updating both phone and watch. */
Pebble.addEventListener('webviewclosed', function(event) {
  var configuration;
  if (!event.response) return;

  try {
    configuration = JSON.parse(decodeURIComponent(event.response));
  } catch (error) {
    console.log('Invalid configuration response: ' + error.message);
    return;
  }

  if (!configuration || typeof configuration.language !== 'number' ||
      (configuration.language !== LANGUAGE_ENGLISH &&
       configuration.language !== LANGUAGE_FRENCH) ||
      typeof configuration.saveStatistics !== 'boolean') {
    console.log('Rejected configuration values or types');
    return;
  }

  localStorage.setItem(STORAGE_KEY_LANGUAGE,
                       String(configuration.language));
  localStorage.setItem(STORAGE_KEY_STATISTICS,
                       configuration.saveStatistics ? '1' : '0');
  sendSettings(false);
});

/* Replaces the mobile mirror only after strict schema and CRC validation. */
Pebble.addEventListener('appmessage', function(event) {
  var bytes = event && event.payload
    ? event.payload[messageKeys.HISTORY_DATA] : null;
  var page;
  var index;
  if (!bytes) return;
  page = decodeHistoryPage(bytes);
  if (page === null) {
    console.log('Rejected corrupt history page');
    return;
  }
  if (page.page === 0) {
    pendingHistory = {
      count:page.count,
      generation:page.generation,
      nextPage:1,
      sessions:page.sessions
    };
  } else if (!pendingHistory || page.generation !== pendingHistory.generation ||
             page.count !== pendingHistory.count ||
             page.page !== pendingHistory.nextPage) {
    console.log('Rejected out-of-order history page');
    return;
  } else {
    for (index = 0; index < page.sessions.length; index += 1) {
      pendingHistory.sessions.push(page.sessions[index]);
    }
    pendingHistory.nextPage += 1;
  }
  if (pendingHistory.sessions.length === pendingHistory.count) {
    localStorage.setItem(STORAGE_KEY_HISTORY,
                         JSON.stringify(pendingHistory.sessions));
    console.log('History mirror updated: ' + pendingHistory.count +
                ' sessions');
    pendingHistory = null;
    openConfiguration();
  }
});

Pebble.addEventListener('ready', function() {
  console.log('Tick Every configuration ready');
  sendSettings(true);
});
