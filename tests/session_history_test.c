#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_history.h"

static unsigned int s_assertion_count;

/* Stops at the first mismatch while preserving the failing source line. */
static void expect_equal(uint64_t actual, uint64_t expected,
                         const char *expression, int line) {
  ++s_assertion_count;
  if (actual != expected) {
    fprintf(stderr, "line %d: %s: expected %llu, got %llu\n", line,
            expression, (unsigned long long)expected,
            (unsigned long long)actual);
    exit(EXIT_FAILURE);
  }
}

#define EXPECT_EQ(actual, expected) \
  expect_equal((uint64_t)(actual), (uint64_t)(expected), #actual, __LINE__)

/* Creates one valid record whose sequence is visible in every field. */
static TickSession make_session(unsigned int value) {
  TickSession session;
  session.ended_at = 1700000000U + value;
  session.total_duration_seconds = value + 20U;
  session.active_duration_seconds = value + 10U;
  session.cycles = value;
  session.interval_seconds = 5U;
  session.delay_seconds = 10U;
  session.flags = value % 2U == 0U ? TICK_SESSION_FLAG_HAPTICS : 0U;
  return session;
}

/* Checks validation before any invalid record can reach persistence. */
static void test_session_validation(void) {
  TickSession session = make_session(1U);
  EXPECT_EQ(tick_session_is_valid(&session), 1U);
  EXPECT_EQ(tick_session_is_valid(NULL), 0U);
  session.ended_at = 0U;
  EXPECT_EQ(tick_session_is_valid(&session), 0U);
  session = make_session(1U);
  session.active_duration_seconds = session.total_duration_seconds + 1U;
  EXPECT_EQ(tick_session_is_valid(&session), 0U);
  session = make_session(1U);
  session.interval_seconds = 31U;
  EXPECT_EQ(tick_session_is_valid(&session), 0U);
  session = make_session(1U);
  session.delay_seconds = 7U;
  EXPECT_EQ(tick_session_is_valid(&session), 0U);
  session = make_session(1U);
  session.flags = 0x80U;
  EXPECT_EQ(tick_session_is_valid(&session), 0U);
}

/* Verifies newest-first insertion, eviction and generation wrap safety. */
static void test_ring(void) {
  TickHistory history;
  unsigned int value;
  tick_history_init(&history);
  EXPECT_EQ(history.count, 0U);

  for (value = 1U; value <= TICK_HISTORY_CAPACITY + 3U; ++value) {
    const TickSession session = make_session(value);
    EXPECT_EQ(tick_history_add(&history, &session), 1U);
  }
  EXPECT_EQ(history.count, TICK_HISTORY_CAPACITY);
  EXPECT_EQ(history.sessions[0].cycles, TICK_HISTORY_CAPACITY + 3U);
  EXPECT_EQ(history.sessions[TICK_HISTORY_CAPACITY - 1U].cycles, 4U);
  EXPECT_EQ(history.generation, TICK_HISTORY_CAPACITY + 3U);
}

/* Covers A/B bank selection, including the uint8 generation wrap. */
static void test_generation_order(void) {
  EXPECT_EQ(tick_history_generation_is_newer(2U, 1U), 1U);
  EXPECT_EQ(tick_history_generation_is_newer(1U, 2U), 0U);
  EXPECT_EQ(tick_history_generation_is_newer(0U, 255U), 1U);
  EXPECT_EQ(tick_history_generation_is_newer(255U, 0U), 0U);
  EXPECT_EQ(tick_history_generation_is_newer(42U, 42U), 0U);
  EXPECT_EQ(tick_history_generation_is_newer(128U, 0U), 0U);
}

/* Round-trips both persistent chunks and rejects any corruption. */
static void test_persistent_chunks(void) {
  TickHistory history;
  TickHistory decoded;
  uint8_t storage[TICK_HISTORY_CHUNK_COUNT][256];
  const uint8_t *chunks[TICK_HISTORY_CHUNK_COUNT];
  size_t sizes[TICK_HISTORY_CHUNK_COUNT];
  unsigned int value;
  unsigned int chunk;

  for (chunk = 0U; chunk < TICK_HISTORY_CHUNK_COUNT; ++chunk) {
    chunks[chunk] = storage[chunk];
    sizes[chunk] = 0U;
  }

  tick_history_init(&history);
  sizes[0] = tick_history_encode_chunk(&history, 0U, storage[0],
                                        sizeof(storage[0]));
  EXPECT_EQ(sizes[0], TICK_HISTORY_HEADER_SIZE);
  EXPECT_EQ(tick_history_chunk_size(&history, 1U), 0U);
  EXPECT_EQ(tick_history_decode_chunks(chunks, sizes, &decoded), 1U);
  EXPECT_EQ(decoded.count, 0U);

  for (value = 1U; value <= TICK_HISTORY_CAPACITY; ++value) {
    const TickSession session = make_session(value);
    EXPECT_EQ(tick_history_add(&history, &session), 1U);
  }
  for (chunk = 0U; chunk < TICK_HISTORY_CHUNK_COUNT; ++chunk) {
    sizes[chunk] = tick_history_encode_chunk(
        &history, chunk, storage[chunk], sizeof(storage[chunk]));
  }
  EXPECT_EQ(sizes[0], 252U);
  EXPECT_EQ(sizes[1], 252U);
  EXPECT_EQ(sizes[2], 172U);
  EXPECT_EQ(tick_history_decode_chunks(chunks, sizes, &decoded), 1U);
  EXPECT_EQ(decoded.count, TICK_HISTORY_CAPACITY);
  EXPECT_EQ(decoded.sessions[0].cycles, TICK_HISTORY_CAPACITY);
  EXPECT_EQ(decoded.sessions[TICK_HISTORY_CAPACITY - 1U].cycles, 1U);

  storage[2][sizes[2] - 1U] ^= 1U;
  EXPECT_EQ(tick_history_decode_chunks(chunks, sizes, &decoded), 0U);
  storage[2][sizes[2] - 1U] ^= 1U;
  storage[1][6] += 1U;
  EXPECT_EQ(tick_history_decode_chunks(chunks, sizes, &decoded), 0U);
}

/* Checks every mobile/persistent page stays below Pebble's 256-byte limit. */
static void test_page_encoding(void) {
  TickHistory history;
  uint8_t page[256];
  unsigned int value;
  unsigned int chunk;

  tick_history_init(&history);
  for (value = 1U; value <= TICK_HISTORY_CAPACITY; ++value) {
    const TickSession session = make_session(value);
    EXPECT_EQ(tick_history_add(&history, &session), 1U);
  }
  for (chunk = 0U; chunk < TICK_HISTORY_CHUNK_COUNT; ++chunk) {
    const size_t size = tick_history_encode_chunk(
        &history, chunk, page, sizeof(page));
    EXPECT_EQ(size <= 256U, 1U);
    EXPECT_EQ(page[0], 'T');
    EXPECT_EQ(page[4], 1U);
    EXPECT_EQ(page[5], TICK_HISTORY_CAPACITY);
    EXPECT_EQ(page[7], chunk);
    EXPECT_EQ(tick_history_encode_chunk(&history, chunk, page, size - 1U),
              0U);
  }
}

int main(void) {
  test_session_validation();
  test_ring();
  test_generation_order();
  test_persistent_chunks();
  test_page_encoding();
  printf("session_history: %u assertions passed\n", s_assertion_count);
  return EXIT_SUCCESS;
}
