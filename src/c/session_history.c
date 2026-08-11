#include "session_history.h"

#include <limits.h>
#include <string.h>

#include "timer_logic.h"

#define HISTORY_VERSION 1U
static const uint8_t HISTORY_MAGIC[4] = {'T', 'E', 'H', '1'};

/* Writes one uint16 in the wire format's explicit little-endian order. */
static void prv_write_u16(uint8_t *buffer, uint16_t value) {
  buffer[0] = (uint8_t)(value & 0xffU);
  buffer[1] = (uint8_t)(value >> 8U);
}

/* Reads one uint16 from the wire format's explicit little-endian order. */
static uint16_t prv_read_u16(const uint8_t *buffer) {
  return (uint16_t)buffer[0] | (uint16_t)((uint16_t)buffer[1] << 8U);
}

/* Writes one uint32 in the wire format's explicit little-endian order. */
static void prv_write_u32(uint8_t *buffer, uint32_t value) {
  buffer[0] = (uint8_t)(value & 0xffU);
  buffer[1] = (uint8_t)((value >> 8U) & 0xffU);
  buffer[2] = (uint8_t)((value >> 16U) & 0xffU);
  buffer[3] = (uint8_t)(value >> 24U);
}

/* Reads one uint32 from the wire format's explicit little-endian order. */
static uint32_t prv_read_u32(const uint8_t *buffer) {
  return (uint32_t)buffer[0] |
         ((uint32_t)buffer[1] << 8U) |
         ((uint32_t)buffer[2] << 16U) |
         ((uint32_t)buffer[3] << 24U);
}

/* Extends a standard CRC-32 across one non-contiguous block. */
static uint32_t prv_crc32_update(uint32_t crc, const uint8_t *data,
                                 size_t size) {
  size_t index;
  unsigned int bit;

  for (index = 0U; index < size; ++index) {
    crc ^= data[index];
    for (bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return crc;
}

/* Covers header metadata and records while deliberately skipping the CRC. */
static uint32_t prv_block_crc(const uint8_t *buffer, size_t size) {
  uint32_t crc = UINT32_MAX;
  crc = prv_crc32_update(crc, buffer, 8U);
  crc = prv_crc32_update(crc, buffer + TICK_HISTORY_HEADER_SIZE,
                         size - TICK_HISTORY_HEADER_SIZE);
  return ~crc;
}

/* Serializes one record without depending on compiler struct padding. */
static void prv_encode_session(const TickSession *session, uint8_t *buffer) {
  prv_write_u32(buffer, session->ended_at);
  prv_write_u32(buffer + 4U, session->total_duration_seconds);
  prv_write_u32(buffer + 8U, session->active_duration_seconds);
  prv_write_u32(buffer + 12U, session->cycles);
  prv_write_u16(buffer + 16U, session->interval_seconds);
  buffer[18] = session->delay_seconds;
  buffer[19] = session->flags;
}

/* Deserializes one record before the caller validates all its ranges. */
static void prv_decode_session(const uint8_t *buffer, TickSession *session) {
  session->ended_at = prv_read_u32(buffer);
  session->total_duration_seconds = prv_read_u32(buffer + 4U);
  session->active_duration_seconds = prv_read_u32(buffer + 8U);
  session->cycles = prv_read_u32(buffer + 12U);
  session->interval_seconds = prv_read_u16(buffer + 16U);
  session->delay_seconds = buffer[18];
  session->flags = buffer[19];
}

/* Encodes the shared versioned header and leaves room for its CRC. */
static void prv_encode_header(uint8_t *buffer, uint8_t count,
                              uint8_t generation, uint8_t marker) {
  memcpy(buffer, HISTORY_MAGIC, sizeof(HISTORY_MAGIC));
  buffer[4] = HISTORY_VERSION;
  buffer[5] = count;
  buffer[6] = generation;
  buffer[7] = marker;
  memset(buffer + 8U, 0, 4U);
}

/* Finalizes the CRC over header metadata and the encoded records. */
static void prv_write_crc(uint8_t *buffer, size_t size) {
  prv_write_u32(buffer + 8U, prv_block_crc(buffer, size));
}

/* Checks the magic, schema, marker, size and CRC of one encoded block. */
static int prv_validate_block(const uint8_t *buffer, size_t size,
                              uint8_t marker, uint8_t expected_records) {
  uint32_t expected_crc;
  uint32_t actual_crc;

  if (buffer == NULL || size != TICK_HISTORY_HEADER_SIZE +
      (size_t)expected_records * TICK_SESSION_SERIALIZED_SIZE ||
      memcmp(buffer, HISTORY_MAGIC, sizeof(HISTORY_MAGIC)) != 0 ||
      buffer[4] != HISTORY_VERSION || buffer[5] > TICK_HISTORY_CAPACITY ||
      buffer[7] != marker) {
    return 0;
  }

  actual_crc = prv_read_u32(buffer + 8U);
  expected_crc = prv_block_crc(buffer, size);
  return actual_crc == expected_crc;
}

/* Resets a history without retaining uninitialized session bytes. */
void tick_history_init(TickHistory *history) {
  if (history != NULL) {
    memset(history, 0, sizeof(*history));
  }
}

/* Treats only the forward half of the uint8 ring as a newer generation. */
int tick_history_generation_is_newer(uint8_t candidate, uint8_t current) {
  const uint8_t difference = (uint8_t)(candidate - current);
  return difference != 0U && difference < 128U;
}

/* Validates every field persisted by Tick Every. */
int tick_session_is_valid(const TickSession *session) {
  if (session == NULL || session->ended_at == 0U ||
      session->active_duration_seconds > session->total_duration_seconds ||
      !tick_timer_is_selectable(session->interval_seconds) ||
      !tick_delay_is_selectable(session->delay_seconds) ||
      (session->flags & (uint8_t)~TICK_SESSION_FLAG_HAPTICS) != 0U) {
    return 0;
  }
  return 1;
}

/* Prepends a session while preserving a small newest-first history. */
int tick_history_add(TickHistory *history, const TickSession *session) {
  size_t retained;

  if (history == NULL || !tick_session_is_valid(session) ||
      history->count > TICK_HISTORY_CAPACITY) {
    return 0;
  }

  retained = history->count < TICK_HISTORY_CAPACITY
      ? history->count : TICK_HISTORY_CAPACITY - 1U;
  if (retained > 0U) {
    memmove(&history->sessions[1], &history->sessions[0],
            retained * sizeof(history->sessions[0]));
  }
  history->sessions[0] = *session;
  if (history->count < TICK_HISTORY_CAPACITY) {
    ++history->count;
  }
  ++history->generation;
  return 1;
}

/* Returns the number of records carried by one persistent chunk. */
static uint8_t prv_chunk_record_count(const TickHistory *history,
                                      unsigned int chunk_index) {
  const unsigned int start = chunk_index * TICK_HISTORY_RECORDS_PER_CHUNK;
  const unsigned int remaining = history != NULL && history->count > start
      ? history->count - start : 0U;

  if (history == NULL || history->count > TICK_HISTORY_CAPACITY) {
    return 0U;
  }
  return (uint8_t)(remaining < TICK_HISTORY_RECORDS_PER_CHUNK
      ? remaining : TICK_HISTORY_RECORDS_PER_CHUNK);
}

/* Returns the exact encoded size for one persistent chunk. */
size_t tick_history_chunk_size(const TickHistory *history,
                               unsigned int chunk_index) {
  if (history == NULL || history->count > TICK_HISTORY_CAPACITY ||
      chunk_index >= TICK_HISTORY_CHUNK_COUNT ||
      (chunk_index > 0U &&
       chunk_index * TICK_HISTORY_RECORDS_PER_CHUNK >= history->count)) {
    return 0U;
  }
  return TICK_HISTORY_HEADER_SIZE +
         (size_t)prv_chunk_record_count(history, chunk_index) *
             TICK_SESSION_SERIALIZED_SIZE;
}

/* Encodes one checksummed chunk within Pebble's 256-byte value limit. */
size_t tick_history_encode_chunk(const TickHistory *history,
                                 unsigned int chunk_index,
                                 uint8_t *buffer, size_t buffer_size) {
  const size_t size = tick_history_chunk_size(history, chunk_index);
  const uint8_t record_count = prv_chunk_record_count(history, chunk_index);
  const unsigned int start = chunk_index * TICK_HISTORY_RECORDS_PER_CHUNK;
  unsigned int index;

  if (size == 0U || buffer == NULL || buffer_size < size) {
    return 0U;
  }
  prv_encode_header(buffer, history->count, history->generation,
                    (uint8_t)chunk_index);
  for (index = 0U; index < record_count; ++index) {
    prv_encode_session(&history->sessions[start + index],
                       buffer + TICK_HISTORY_HEADER_SIZE +
                           (size_t)index * TICK_SESSION_SERIALIZED_SIZE);
  }
  prv_write_crc(buffer, size);
  return size;
}

/* Decodes a validated block's records into the requested destination range. */
static int prv_decode_records(const uint8_t *buffer, uint8_t record_count,
                              unsigned int start, TickHistory *history) {
  unsigned int index;
  for (index = 0U; index < record_count; ++index) {
    TickSession session;
    prv_decode_session(buffer + TICK_HISTORY_HEADER_SIZE +
                           (size_t)index * TICK_SESSION_SERIALIZED_SIZE,
                       &session);
    if (!tick_session_is_valid(&session)) {
      return 0;
    }
    history->sessions[start + index] = session;
  }
  return 1;
}

/* Loads every required page only when it describes one coherent generation. */
int tick_history_decode_chunks(
    const uint8_t *chunks[TICK_HISTORY_CHUNK_COUNT],
    const size_t sizes[TICK_HISTORY_CHUNK_COUNT], TickHistory *history) {
  uint8_t count;
  unsigned int chunk_index;

  if (history == NULL || chunks == NULL || sizes == NULL ||
      chunks[0] == NULL || sizes[0] < TICK_HISTORY_HEADER_SIZE) {
    return 0;
  }
  count = chunks[0][5];
  if (count > TICK_HISTORY_CAPACITY) {
    return 0;
  }
  if (!prv_validate_block(chunks[0], sizes[0], 0U,
                          count < TICK_HISTORY_RECORDS_PER_CHUNK
                              ? count : TICK_HISTORY_RECORDS_PER_CHUNK)) {
    return 0;
  }

  tick_history_init(history);
  history->count = count;
  history->generation = chunks[0][6];
  for (chunk_index = 0U; chunk_index < TICK_HISTORY_CHUNK_COUNT;
       ++chunk_index) {
    const unsigned int start = chunk_index * TICK_HISTORY_RECORDS_PER_CHUNK;
    const uint8_t record_count = count > start
        ? (uint8_t)((count - start) < TICK_HISTORY_RECORDS_PER_CHUNK
            ? count - start : TICK_HISTORY_RECORDS_PER_CHUNK)
        : 0U;
    if (chunk_index > 0U && record_count == 0U) {
      break;
    }
    if (chunks[chunk_index] == NULL ||
        sizes[chunk_index] < TICK_HISTORY_HEADER_SIZE ||
        chunks[chunk_index][5] != count ||
        chunks[chunk_index][6] != history->generation ||
        !prv_validate_block(chunks[chunk_index], sizes[chunk_index],
                            (uint8_t)chunk_index, record_count) ||
        !prv_decode_records(chunks[chunk_index], record_count, start,
                            history)) {
      return 0;
    }
  }
  return 1;
}
