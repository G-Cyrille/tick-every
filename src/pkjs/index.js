/* Initialise le pont PebbleKit JS quand la connexion au téléphone est prête. */
Pebble.addEventListener('ready', function() {
  console.log('PebbleKit JS ready');
});

/* Journalise les messages reçus de la montre pendant le développement. */
Pebble.addEventListener('appmessage', function(event) {
  console.log('AppMessage received: ' + JSON.stringify(event.payload));
});
