var messageKeys = require('message_keys');

var CONFIG_URL = 'https://s3.grossholtz.net/public/tick-every/config/2026/08/b6e5dc60-4cdb-447e-9f9f-54c41ea8a9a1-config.html';
var STORAGE_KEY_LANGUAGE = 'tick-every-language';
var STORAGE_KEY_STATISTICS = 'tick-every-save-statistics';
var STORAGE_KEY_HISTORY = 'tick-every-session-history';
var STORAGE_KEY_HISTORY_V2 = 'tick-every-session-history-v2';
var LANGUAGE_ENGLISH = 0;
var LANGUAGE_FRENCH = 1;
var WATCH_HISTORY_CAPACITY = 32;
var MOBILE_HISTORY_PAGE_SIZE = 32;
var MOBILE_HISTORY_STORAGE_VERSION = 2;
var HISTORY_HEADER_SIZE = 12;
var HISTORY_RECORD_SIZE = 20;
var HISTORY_RECORDS_PER_PAGE = 12;
var HISTORY_PAGE_COUNT = 3;
var SETTINGS_RETRY_COUNT = 3;
var SETTINGS_RETRY_DELAY_MS = 500;
var configurationOpenPending = false;
var configurationOpenTimer = null;
var configurationHistoryOffset = 0;
var configurationDraftLanguage = null;
var configurationDraftStatistics = null;
var mobileHistoryStorageWarning = false;
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

/* Expands the compact on-phone format while accepting the 1.1 object format. */
function expandStoredSession(value) {
  if (Array.isArray(value) && value.length === 7) {
    return {
      endedAt:value[0], totalDuration:value[1], activeDuration:value[2],
      cycles:value[3], interval:value[4], delay:value[5],
      haptics:value[6] === 1
    };
  }
  return value;
}

/* Filters either compact or legacy object records through the strict schema. */
function validStoredSessions(values) {
  var valid = [];
  var index;
  var session;
  if (!Array.isArray(values)) return valid;
  for (index = 0; index < values.length; index += 1) {
    session = expandStoredSession(values[index]);
    if (isValidSession(session)) valid.push(session);
  }
  return valid;
}

/* Loads the atomic v2 archive, or migrates the 1.1 mirror without deleting it. */
function getStoredHistoryState() {
  var parsed;
  try {
    parsed = JSON.parse(localStorage.getItem(STORAGE_KEY_HISTORY_V2) || 'null');
  } catch (error) {
    parsed = null;
  }
  if (parsed && parsed.version === MOBILE_HISTORY_STORAGE_VERSION &&
      Array.isArray(parsed.archive) && Array.isArray(parsed.watchSnapshot)) {
    return {
      archive:validStoredSessions(parsed.archive),
      watchSnapshot:validStoredSessions(parsed.watchSnapshot).slice(
        0, WATCH_HISTORY_CAPACITY),
      watchGeneration:typeof parsed.watchGeneration === 'number' &&
        parsed.watchGeneration >= 0 && parsed.watchGeneration <= 255
        ? parsed.watchGeneration : null
    };
  }

  try {
    parsed = JSON.parse(localStorage.getItem(STORAGE_KEY_HISTORY) || '[]');
  } catch (legacyError) {
    parsed = [];
  }
  parsed = validStoredSessions(parsed);
  return {
    archive:parsed,
    watchSnapshot:parsed.slice(0, WATCH_HISTORY_CAPACITY),
    watchGeneration:null
  };
}

/* Returns every valid locally archived record; the app imposes no count cap. */
function getStoredHistory() {
  return getStoredHistoryState().archive;
}

/* Compares records only inside an ordered snapshot overlap. */
function sessionKey(session) {
  return compactSession(session).join(':');
}

function sessionsEqual(left, right) {
  return sessionKey(left) === sessionKey(right);
}

/* Finds the newest prefix added since the prior watch snapshot. */
function newWatchSessions(watchSessions, generation, previous,
                          previousGeneration) {
  var delta;
  var overlap;
  var index;
  var added;
  var bestAdded = watchSessions.length;
  var bestOverlap = 0;
  var matches;

  if (typeof previousGeneration === 'number') {
    delta = (generation - previousGeneration + 256) % 256;
    if (delta === 0 && watchSessions.length === previous.length) {
      matches = true;
      for (index = 0; index < watchSessions.length; index += 1) {
        if (!sessionsEqual(watchSessions[index], previous[index])) {
          matches = false;
          break;
        }
      }
      if (matches) return [];
    }
    if (delta > 0 && delta <= WATCH_HISTORY_CAPACITY &&
        delta <= watchSessions.length) {
      overlap = Math.min(watchSessions.length - delta, previous.length);
      matches = overlap > 0 || delta === watchSessions.length;
      for (index = 0; matches && index < overlap; index += 1) {
        if (!sessionsEqual(watchSessions[delta + index], previous[index])) {
          matches = false;
        }
      }
      if (matches) return watchSessions.slice(0, delta);
    }
    /* A gap beyond the watch capacity cannot be reconstructed safely. */
    return watchSessions.slice(0);
  }

  /* Only legacy 1.1 data lacks a generation; migrate it by ordered overlap. */
  for (added = 0; added <= watchSessions.length; added += 1) {
    overlap = Math.min(watchSessions.length - added, previous.length);
    if (overlap === 0) continue;
    matches = true;
    for (index = 0; index < overlap; index += 1) {
      if (!sessionsEqual(watchSessions[added + index], previous[index])) {
        matches = false;
        break;
      }
    }
    if (matches && (overlap > bestOverlap ||
        (overlap === bestOverlap && added > bestAdded))) {
      bestOverlap = overlap;
      bestAdded = added;
    }
  }
  return bestOverlap > 0 ? watchSessions.slice(0, bestAdded)
    : watchSessions.slice(0);
}

/* Adds only the newly observed prefix without deleting the older archive. */
function mergeWatchHistory(state, watchSessions, generation) {
  var additions = newWatchSessions(watchSessions, generation,
                                    state.watchSnapshot,
                                    state.watchGeneration);
  return additions.concat(state.archive);
}

/* Compacts a list before its single atomic localStorage write. */
function compactSessions(sessions) {
  var compact = [];
  var index;
  for (index = 0; index < sessions.length; index += 1) {
    compact.push(compactSession(sessions[index]));
  }
  return compact;
}

/* Stores archive and sync cursor atomically; quota failure changes neither. */
function storeHistoryState(history, watchSnapshot, watchGeneration) {
  try {
    localStorage.setItem(STORAGE_KEY_HISTORY_V2, JSON.stringify({
      version:MOBILE_HISTORY_STORAGE_VERSION,
      archive:compactSessions(history),
      watchSnapshot:compactSessions(watchSnapshot),
      watchGeneration:watchGeneration
    }));
    mobileHistoryStorageWarning = false;
    return true;
  } catch (error) {
    mobileHistoryStorageWarning = true;
    console.log('Mobile history storage failed: ' + error.message);
    return false;
  }
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
  if (count > WATCH_HISTORY_CAPACITY || (page > 0 && recordCount === 0) ||
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

/* Compacts one record so one mobile page remains safe for old webview URLs. */
function compactSession(session) {
  return [session.endedAt, session.totalDuration, session.activeDuration,
    session.cycles, session.interval, session.delay, session.haptics ? 1 : 0];
}

/* Opens the static page with a compact local fragment absent from HTTP logs. */
function openConfiguration() {
  var state;
  var history;
  var page;
  var maximumOffset;
  if (!configurationOpenPending) return;
  configurationOpenPending = false;
  if (configurationOpenTimer !== null) {
    clearTimeout(configurationOpenTimer);
    configurationOpenTimer = null;
  }
  history = getStoredHistory();
  maximumOffset = history.length > 0
    ? Math.floor((history.length - 1) / MOBILE_HISTORY_PAGE_SIZE) *
        MOBILE_HISTORY_PAGE_SIZE
    : 0;
  if (configurationHistoryOffset > maximumOffset) {
    configurationHistoryOffset = maximumOffset;
  }
  page = history.slice(configurationHistoryOffset,
                       configurationHistoryOffset + MOBILE_HISTORY_PAGE_SIZE);
  state = {
    l:configurationDraftLanguage === null ? getStoredLanguage()
      : configurationDraftLanguage,
    s:configurationDraftStatistics === null ? getStoredStatisticsEnabled()
      : configurationDraftStatistics,
    h:[], o:configurationHistoryOffset, n:history.length,
    w:mobileHistoryStorageWarning
  };
  page.forEach(function(session) {
    state.h.push(compactSession(session));
  });
  Pebble.openURL(CONFIG_URL + '#state=' +
    encodeURIComponent(JSON.stringify(state)));
}

/* Requests a fresh snapshot but never blocks opening when the watch is absent. */
Pebble.addEventListener('showConfiguration', function() {
  configurationHistoryOffset = 0;
  configurationDraftLanguage = getStoredLanguage();
  configurationDraftStatistics = getStoredStatisticsEnabled();
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

  if (configuration && configuration.action === 'page') {
    if (typeof configuration.historyOffset !== 'number' ||
        configuration.historyOffset % 1 !== 0 ||
        configuration.historyOffset < 0 ||
        configuration.historyOffset % MOBILE_HISTORY_PAGE_SIZE !== 0 ||
        (configuration.language !== LANGUAGE_ENGLISH &&
         configuration.language !== LANGUAGE_FRENCH) ||
        typeof configuration.saveStatistics !== 'boolean') {
      console.log('Rejected history page offset');
      return;
    }
    configurationHistoryOffset = configuration.historyOffset;
    configurationDraftLanguage = configuration.language;
    configurationDraftStatistics = configuration.saveStatistics;
    configurationOpenPending = true;
    openConfiguration();
    return;
  }

  if (!configuration ||
      (configuration.action !== undefined && configuration.action !== 'save') ||
      typeof configuration.language !== 'number' ||
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
  configurationDraftLanguage = null;
  configurationDraftStatistics = null;
  sendSettings(false);
});

/* Merges a complete watch snapshot into the longer on-phone archive. */
/* The mobile app delivers payload keys by name; the emulator by number. */
Pebble.addEventListener('appmessage', function(event) {
  var bytes = event && event.payload
    ? event.payload.HISTORY_DATA || event.payload[messageKeys.HISTORY_DATA]
    : null;
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
    var storedState = getStoredHistoryState();
    var mergedHistory = mergeWatchHistory(storedState,
                                          pendingHistory.sessions,
                                          pendingHistory.generation);
    if (storeHistoryState(mergedHistory, pendingHistory.sessions,
                          pendingHistory.generation)) {
      console.log('Mobile history updated: ' + mergedHistory.length +
                  ' sessions (' + pendingHistory.count + ' on watch)');
    }
    pendingHistory = null;
    openConfiguration();
  }
});

Pebble.addEventListener('ready', function() {
  console.log('Tick Every configuration ready');
  sendSettings(true);
});
