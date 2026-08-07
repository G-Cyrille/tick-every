'use strict';

var fs = require('fs');
var vm = require('vm');

var assertions = 0;
var handlers = {};
var openedUrls = [];
var sentLanguages = [];
var storage = {};
var failNextSend = false;

function assert(condition, message) {
  assertions += 1;
  if (!condition) {
    throw new Error(message);
  }
}

var context = {
  require: function() {
    return {LANGUAGE: 10000};
  },
  console: {log: function() {}},
  decodeURIComponent: decodeURIComponent,
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
      sentLanguages.push(payload[10000]);
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

handlers.showConfiguration();
assert(/language=0$/.test(openedUrls[0]), 'English must be preselected by default');

[null, false, true, '', '1', 2].forEach(function(value) {
  handlers.webviewclosed({
    response: encodeURIComponent(JSON.stringify({language: value}))
  });
});
assert(sentLanguages.length === 0, 'malformed language value was accepted');

handlers.webviewclosed({response: '%'});
handlers.webviewclosed({response: encodeURIComponent('{bad json')});
assert(sentLanguages.length === 0, 'malformed configuration was accepted');

handlers.webviewclosed({
  response: encodeURIComponent(JSON.stringify({language: 1}))
});
assert(sentLanguages.join(',') === '1', 'French was not sent');
assert(storage['tick-every-language'] === '1', 'French was not stored');

handlers.ready();
assert(sentLanguages.join(',') === '1,1', 'stored language was not synced on ready');

failNextSend = true;
handlers.webviewclosed({
  response: encodeURIComponent(JSON.stringify({language: 0}))
});
assert(storage['tick-every-language'] === '0',
       'choice must survive a temporary delivery failure');

handlers.ready();
assert(sentLanguages.join(',') === '1,1,0,0',
       'failed language delivery was not retried on ready');

handlers.showConfiguration();
assert(/language=0$/.test(openedUrls[1]), 'stored English was not preselected');

console.log('pkjs_language: ' + assertions + ' assertions passed');
