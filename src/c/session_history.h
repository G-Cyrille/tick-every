#ifndef SESSION_HISTORY_H
#define SESSION_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#define TICK_HISTORY_CAPACITY 32U
#define TICK_SESSION_SERIALIZED_SIZE 20U
#define TICK_HISTORY_HEADER_SIZE 12U
#define TICK_HISTORY_RECORDS_PER_CHUNK 12U
#define TICK_HISTORY_CHUNK_COUNT 3U

#define TICK_SESSION_FLAG_HAPTICS 0x01U

/* One completed timer session, using only fixed-width portable fields. */
typedef struct {
  uint32_t ended_at;
  uint32_t total_duration_seconds;
  uint32_t active_duration_seconds;
  uint32_t cycles;
  uint16_t interval_seconds;
  uint8_t delay_seconds;
  uint8_t flags;
} TickSession;

/* Newest session is always at index zero; the oldest is evicted when full. */
typedef struct {
  TickSession sessions[TICK_HISTORY_CAPACITY];
  uint8_t count;
  uint8_t generation;
} TickHistory;

/* Resets a history without retaining uninitialized session bytes. */
void tick_history_init(TickHistory *history);

/* Compares adjacent uint8 generations correctly across wrap-around. */
int tick_history_generation_is_newer(uint8_t candidate, uint8_t current);

/* Validates ranges shared by persistent storage and AppMessage decoding. */
int tick_session_is_valid(const TickSession *session);

/* Prepends one valid session and evicts the oldest record when necessary. */
int tick_history_add(TickHistory *history, const TickSession *session);

/* Returns the exact encoded size for one of the two persistent chunks. */
size_t tick_history_chunk_size(const TickHistory *history,
                               unsigned int chunk_index);

/* Encodes a checksummed persistent chunk; returns zero on invalid arguments. */
size_t tick_history_encode_chunk(const TickHistory *history,
                                 unsigned int chunk_index,
                                 uint8_t *buffer, size_t buffer_size);

/* Loads and validates all chunks required by one complete generation. */
int tick_history_decode_chunks(
    const uint8_t *chunks[TICK_HISTORY_CHUNK_COUNT],
    const size_t sizes[TICK_HISTORY_CHUNK_COUNT], TickHistory *history);

#endif
