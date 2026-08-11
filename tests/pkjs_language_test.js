'use strict';

var fs = require('fs');
var vm = require('vm');

var LANGUAGE = 10000;
var SAVE_STATISTICS = 10001;
var HISTORY_REQUEST = 10002;
var HISTORY_DATA = 10003;
var assertions = 0;
var handlers = {};
var openedUrls = [];
var sentPayloads = [];
var storage = {};
var timers = [];
var failNextSend = false;

function assert(condition, message) {
  assertions += 1;
  if (!condition) throw new Error(message);
}

function writeUint32(bytes, offset, value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >>> 8) & 0xff;
  bytes[offset + 2] = (value >>> 16) & 0xff;
  bytes[offset + 3] = (value >>> 24) & 0xff;
}

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

function historyCrc(bytes) {
  var crc = 0xffffffff;
  crc = crc32Update(crc, bytes, 0, 8);
  crc = crc32Update(crc, bytes, 12, bytes.length);
  return (~crc) >>> 0;
}

function makeHistoryPage(sessions, total, page, generation) {
  var bytes = [];
  var index;
  var offset;
  var session;
  var flags;
  for (index = 0; index < 12 + sessions.length * 20; index += 1) {
    bytes[index] = 0;
  }
  bytes[0] = 84;
  bytes[1] = 69;
  bytes[2] = 72;
  bytes[3] = 49;
  bytes[4] = 1;
  bytes[5] = total;
  bytes[6] = generation;
  bytes[7] = page;
  for (index = 0; index < sessions.length; index += 1) {
    session = sessions[index];
    flags = session.haptics ? 1 : 0;
    offset = 12 + index * 20;
    writeUint32(bytes, offset, session.endedAt);
    writeUint32(bytes, offset + 4, session.totalDuration);
    writeUint32(bytes, offset + 8, session.activeDuration);
    writeUint32(bytes, offset + 12, session.cycles);
    bytes[offset + 16] = session.interval & 0xff;
    bytes[offset + 17] = (session.interval >>> 8) & 0xff;
    bytes[offset + 18] = session.delay;
    bytes[offset + 19] = flags;
  }
  writeUint32(bytes, 8, historyCrc(bytes));
  return bytes;
}

function makeHistory(session) {
  return makeHistoryPage([session], 1, 0, 7);
}

function parseOpenedState(index) {
  var marker = '#state=';
  var url = openedUrls[index];
  return JSON.parse(decodeURIComponent(url.slice(url.indexOf(marker) +
    marker.length)));
}

var context = {
  require: function() {
    return {
      LANGUAGE: LANGUAGE,
      SAVE_STATISTICS: SAVE_STATISTICS,
      HISTORY_REQUEST: HISTORY_REQUEST,
      HISTORY_DATA: HISTORY_DATA
    };
  },
  console: {log: function() {}},
  decodeURIComponent: decodeURIComponent,
  encodeURIComponent: encodeURIComponent,
  JSON: JSON,
  Array: Array,
  clearTimeout: function(id) {
    timers[id] = null;
  },
  setTimeout: function(callback) {
    timers.push(callback);
    return timers.length - 1;
  },
  localStorage: {
    getItem: function(key) {
      return Object.prototype.hasOwnProperty.call(storage, key)
        ? storage[key] : null;
    },
    setItem: function(key, value) {
      storage[key] = value;
    }
  },
  Pebble: {
    addEventListener: function(name, callback) {
      handlers[name] = callback;
    },
    openURL: function(url) {
      openedUrls.push(url);
    },
    sendAppMessage: function(payload, success, failure) {
      sentPayloads.push(payload);
      if (failNextSend) {
        failNextSend = false;
        failure({error: 'simulated'});
      } else {
        success();
      }
    }
  }
};

vm.runInNewContext(fs.readFileSync('src/pkjs/index.js', 'utf8'), context);

assert(typeof handlers.ready === 'function', 'ready handler missing');
assert(typeof handlers.showConfiguration === 'function',
       'showConfiguration handler missing');
assert(typeof handlers.webviewclosed === 'function',
       'webviewclosed handler missing');
assert(typeof handlers.appmessage === 'function', 'appmessage handler missing');

handlers.showConfiguration();
assert(openedUrls.length === 0, 'configuration opened before sync or timeout');
assert(sentPayloads[0][LANGUAGE] === 0, 'English must be the default');
assert(sentPayloads[0][SAVE_STATISTICS] === 0,
       'statistics must be opt-in');
assert(sentPayloads[0][HISTORY_REQUEST] === 1,
       'configuration must request fresh history');
timers[0]();
assert(openedUrls.length === 1, 'timeout did not open configuration');
var state = parseOpenedState(0);
assert(state.l === 0, 'English was not preselected');
assert(state.s === false, 'statistics were not preselected off');
assert(state.h.length === 0, 'default history must be empty');

[null, false, true, '', '1', 2].forEach(function(value) {
  handlers.webviewclosed({
    response: encodeURIComponent(JSON.stringify({
      language: value,
      saveStatistics: false
    }))
  });
});
handlers.webviewclosed({
  response: encodeURIComponent(JSON.stringify({language: 1}))
});
handlers.webviewclosed({response: '%'});
handlers.webviewclosed({response: encodeURIComponent('{bad json')});
assert(sentPayloads.length === 1, 'malformed configuration was accepted');

handlers.webviewclosed({
  response: encodeURIComponent(JSON.stringify({
    language: 1,
    saveStatistics: true
  }))
});
assert(sentPayloads[1][LANGUAGE] === 1, 'French was not sent');
assert(sentPayloads[1][SAVE_STATISTICS] === 1,
       'statistics setting was not sent');
assert(sentPayloads[1][HISTORY_REQUEST] === undefined,
       'saving settings must not request history');
assert(storage['tick-every-language'] === '1', 'French was not stored');
assert(storage['tick-every-save-statistics'] === '1',
       'statistics choice was not stored');

var session = {
  endedAt: 1786454100,
  totalDuration: 75,
  activeDuration: 60,
  cycles: 12,
  interval: 5,
  delay: 10,
  haptics: true
};
handlers.showConfiguration();
handlers.appmessage({payload: (function() {
  var payload = {};
  payload[HISTORY_DATA] = makeHistory(session);
  return payload;
}())});
assert(openedUrls.length === 2,
       'fresh history did not open pending configuration');
state = parseOpenedState(1);
assert(state.l === 1 && state.s === true,
       'stored settings missing from configuration state');
assert(state.h.length === 1, 'history record missing');
assert(state.h[0][0] === session.endedAt,
       'history timestamp decoded incorrectly');
assert(state.h[0][3] === 12 && state.h[0][6] === 1,
       'history metadata decoded incorrectly');
assert(openedUrls[1].length < 1200,
       'compact single-session configuration URL is unexpectedly large');
assert(JSON.parse(storage['tick-every-session-history']).length === 1,
       'mobile history mirror was not stored');

var corrupt = makeHistory(session);
corrupt[16] ^= 1;
handlers.appmessage({payload: (function() {
  var payload = {};
  payload[HISTORY_DATA] = corrupt;
  return payload;
}())});
assert(JSON.parse(storage['tick-every-session-history'])[0].totalDuration === 75,
       'corrupt history replaced the valid mirror');

var pageSessions = [];
while (pageSessions.length < 32) pageSessions.push(session);
var page0 = makeHistoryPage(pageSessions.slice(0, 12), 32, 0, 8);
var page1 = makeHistoryPage(pageSessions.slice(12, 24), 32, 1, 8);
var page2 = makeHistoryPage(pageSessions.slice(24, 32), 32, 2, 8);
function deliverHistory(bytes) {
  var payload = {};
  payload[HISTORY_DATA] = bytes;
  handlers.appmessage({payload:payload});
}
deliverHistory(page0);
assert(JSON.parse(storage['tick-every-session-history']).length === 1,
       'partial snapshot replaced the complete mirror');
deliverHistory(page2);
assert(JSON.parse(storage['tick-every-session-history']).length === 1,
       'out-of-order page replaced the complete mirror');
deliverHistory(page1);
deliverHistory(page2);
assert(JSON.parse(storage['tick-every-session-history']).length === 32,
       'three-page snapshot was not assembled');

handlers.ready();
assert(sentPayloads[sentPayloads.length - 1][LANGUAGE] === 1,
       'stored language was not synced on ready');
assert(sentPayloads[sentPayloads.length - 1][SAVE_STATISTICS] === 1,
       'stored statistics setting was not synced on ready');
assert(sentPayloads[sentPayloads.length - 1][HISTORY_REQUEST] === 1,
       'ready must request history');

failNextSend = true;
handlers.webviewclosed({
  response: encodeURIComponent(JSON.stringify({
    language: 0,
    saveStatistics: false
  }))
});
assert(storage['tick-every-language'] === '0',
       'language choice must survive temporary delivery failure');
assert(storage['tick-every-save-statistics'] === '0',
       'statistics choice must survive temporary delivery failure');
var retryTimer = null;
timers.forEach(function(timer) {
  if (typeof timer === 'function') retryTimer = timer;
});
assert(typeof retryTimer === 'function', 'failed settings were not queued');
retryTimer();
assert(sentPayloads[sentPayloads.length - 1][LANGUAGE] === 0 &&
       sentPayloads[sentPayloads.length - 1][SAVE_STATISTICS] === 0,
       'failed settings were not retried');

var fullHistory = [];
while (fullHistory.length < 32) fullHistory.push(session);
storage['tick-every-session-history'] = JSON.stringify(fullHistory);
handlers.showConfiguration();
var fullHistoryTimer = null;
timers.forEach(function(timer) {
  if (typeof timer === 'function') fullHistoryTimer = timer;
});
fullHistoryTimer();
assert(parseOpenedState(openedUrls.length - 1).h.length === 32,
       'full history was not passed to configuration');
assert(openedUrls[openedUrls.length - 1].length < 4000,
       'full compact history exceeds the conservative URL budget');

console.log('pkjs_config: ' + assertions + ' assertions passed');
