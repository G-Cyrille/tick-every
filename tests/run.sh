#!/bin/sh
set -eu

test_binary="${TMPDIR:-/tmp}/tick-every-timer-logic-test"
history_test_binary="${TMPDIR:-/tmp}/tick-every-session-history-test"

cc -std=c99 -Wall -Wextra -Werror -pedantic \
  -Isrc/c src/c/timer_logic.c tests/timer_logic_test.c \
  -o "$test_binary"
"$test_binary"

cc -std=c99 -Wall -Wextra -Werror -pedantic \
  -Isrc/c src/c/timer_logic.c src/c/session_history.c \
  tests/session_history_test.c -o "$history_test_binary"
"$history_test_binary"
node tests/pkjs_language_test.js
