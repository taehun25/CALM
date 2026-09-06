/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/time.h"
#include "dds/ddsi/ddsi_entity_match.h"
#include "dds/ddsi/ddsi_entity_index.h"
#include "dds/ddsi/ddsi_domaingv.h"
#include "dds/ddsi/ddsi_endpoint.h"
#include "dds/ddsi/ddsi_proxy_endpoint.h"
#include "dds/ddsi/ddsi_storm_observer.h"
#include "dds/ddsi/q_hbcontrol.h"
#include "dds/ddsi/q_rtps.h"
#include "dds/ddsi/q_transmit.h"
#include "dds/ddsi/q_whc.h"
#include "dds/ddsi/q_xevent.h"

struct ddsi_storm_repair_entry
{
  struct ddsi_storm_repair_entry *next;
  seqno_t seq;
  uint32_t sample_size;
  uint32_t fragment_size;
  uint32_t nfrags;
  uint8_t *outstanding;
  uint8_t *pending;
  uint8_t *sent_in_generation;
  ddsrt_mtime_t first_nack_time;
  ddsrt_mtime_t generation_first_retransmit_time;
  ddsrt_mtime_t last_retransmit_time;
  uint32_t retransmit_count;
  uint32_t failed_repair_count;
  uint32_t request_generation;
  uint32_t sent_generation;
  uint32_t failed_generation;
  uint64_t feedback_requested_bytes;
  uint64_t feedback_rerequested_sent_bytes;
  uint64_t previous_feedback_requested_bytes;
  bool previous_feedback_requested_valid;
  bool nack_seen;
};

struct ddsi_storm_held_new_entry
{
  struct ddsi_storm_held_new_entry *next;
  seqno_t seq;
  uint32_t sample_size;
  uint32_t next_fragment;
};

struct ddsi_storm_reader_state
{
  struct ddsi_domaingv *gv;
  ddsi_guid_t writer_guid;
  ddsi_guid_t reader_guid;
  struct xevent *pacing_event;
  struct ddsi_storm_repair_entry *repairs;
  struct ddsi_storm_held_new_entry *held_new;
  uint64_t rho_bytes;
  uint64_t nack_events_total;
  uint64_t nack_bytes_total;
  uint64_t unique_repair_bytes_total;
  uint64_t repeated_nack_events_total;
  uint64_t repeated_nack_bytes_total;
  uint64_t schedule_events_total;
  uint64_t schedule_requested_changes_total;
  uint64_t schedule_requested_bytes_total;
  uint64_t enqueue_accepted_changes_total;
  uint64_t enqueue_accepted_bytes_total;
  uint64_t enqueue_rejected_changes_total;
  uint64_t enqueue_rejected_bytes_total;
  uint64_t transport_repair_attempt_events_total;
  uint64_t transport_repair_attempt_bytes_total;
  uint64_t ack_recovered_bytes_total;
  seqno_t reader_acked_high_seq;
  seqno_t oldest_no_progress_seq;
  uint32_t oldest_no_progress_rounds;
  uint64_t observed_sample_bytes;
  uint64_t observed_sample_count;
  double mean_sample_bytes;
  double offered_rate_bps;
  double service_rate_bps;
  double service_rate_at_entry_bps;
  double release_rate_bps;
  double release_rate_min_bps;
  double release_rate_max_bps;
  double release_rate_ai_bps;
  ddsrt_mtime_t last_write_time;
  ddsrt_mtime_t last_ack_progress_time;
  ddsrt_mtime_t last_service_sample_time;
  ddsrt_mtime_t last_repair_time;
  uint64_t ack_window_bytes;
  uint64_t offered_window_bytes;
  uint64_t previous_feedback_rho_bytes;
  uint64_t feedback_round_start_u_bytes;
  uint64_t feedback_round_released_bytes;
  uint32_t feedback_round_start_oldest_failed_count;
  int64_t last_feedback_delta_u_bytes;
  uint32_t last_feedback_delta_oldest_failed;
  double last_feedback_progress_fraction;
  uint64_t budget_bytes;
  uint64_t budget_min_bytes;
  uint64_t budget_max_bytes;
  uint64_t budget_ai_bytes;
  uint64_t pending_repair_bytes;
  uint64_t held_new_bytes;
  uint64_t release_credit;
  uint64_t last_release_bytes;
  uint32_t nonshrinking_rounds;
  uint32_t repair_round;
  uint64_t last_feedback_requested_bytes;
  uint64_t last_feedback_previous_requested_bytes;
  uint64_t last_feedback_decrease_bytes;
  double last_feedback_residual_ratio;
  double last_failure_severity;
  double last_effective_budget_gamma;
  double last_effective_rate_gamma;
  uint32_t last_feedback_progressing_changes;
  uint32_t last_feedback_stalled_changes;
  bool repair_round_armed;
  bool calm_active;
};

struct ddsi_calm_config
{
  bool enabled;
  double rho_sample_multiplier;
  uint32_t growth_rounds;
  int64_t ack_stall_ns;
  int64_t feedback_guard_ns;
  int64_t pacing_floor_ns;
  double pacing_drain_guard;
  double decrease_gain;
  double increase_gain;
  double min_budget_sample_multiplier;
  double initial_budget_sample_multiplier;
  double max_budget_sample_multiplier;
  double ai_budget_sample_multiplier;
  uint64_t min_budget_floor;
  uint64_t initial_budget_floor;
  uint64_t max_budget_floor;
  uint64_t ai_budget_floor;
  double first_failure_gamma;
  double repeated_failure_gamma;
  double first_failure_rate_gamma;
  double repeated_failure_rate_gamma;
  double offered_alpha;
  double service_alpha_up;
  double service_alpha_down;
  double service_failure_gamma;
  double service_min_multiplier;
  double service_initial_multiplier;
  double service_max_multiplier;
  double service_ai_multiplier;
  bool feedback_byte_progress;
};

static ddsrt_once_t storm_once = DDSRT_ONCE_INIT;
static ddsrt_mutex_t storm_log_lock;
static FILE *storm_log_file;
static bool storm_enabled;
static struct ddsi_calm_config calm_cfg;
static uint32_t storm_rows_since_flush;

static bool env_enabled (const char *value)
{
  return value != NULL && value[0] != '\0' && strcmp (value, "0") != 0 &&
         strcmp (value, "false") != 0 && strcmp (value, "off") != 0 &&
         strcmp (value, "no") != 0;
}

static double env_double (const char *name, double fallback)
{
  const char *value = getenv (name);
  char *end;
  double parsed;
  if (value == NULL || value[0] == '\0')
    return fallback;
  parsed = strtod (value, &end);
  return end != value && *end == '\0' && isfinite (parsed) ? parsed : fallback;
}

static uint64_t env_uint64 (const char *name, uint64_t fallback)
{
  const char *value = getenv (name);
  char *end;
  unsigned long long parsed;
  if (value == NULL || value[0] == '\0')
    return fallback;
  parsed = strtoull (value, &end, 10);
  return end != value && *end == '\0' ? (uint64_t) parsed : fallback;
}

static uint32_t env_uint32 (const char *name, uint32_t fallback)
{
  const uint64_t value = env_uint64 (name, fallback);
  return value > UINT32_MAX ? fallback : (uint32_t) value;
}

static uint64_t max_u64 (uint64_t a, uint64_t b)
{
  return a > b ? a : b;
}

static double max_double (double a, double b)
{
  return a > b ? a : b;
}

static void storm_init (void)
{
  const char *enabled = getenv ("CYCLONEDDS_STORM_OBSERVER_ENABLED");
  const char *path = getenv ("CYCLONEDDS_STORM_LOG_FILE");

  ddsrt_mutex_init (&storm_log_lock);
  storm_enabled = env_enabled (enabled);
  calm_cfg.enabled = env_enabled (getenv ("CYCLONEDDS_CALM_ENABLED"));
  calm_cfg.rho_sample_multiplier = env_double (
    "CYCLONEDDS_CALM_RHO_SAMPLE_MULTIPLIER", 2.0);
  calm_cfg.growth_rounds = env_uint32 (
    "CYCLONEDDS_CALM_GROWTH_ROUNDS", 2);
  calm_cfg.ack_stall_ns = (int64_t) (env_double (
    "CYCLONEDDS_CALM_ACK_STALL_MS", 500.0) * 1000000.0);
  calm_cfg.feedback_guard_ns = (int64_t) (env_double (
    "CYCLONEDDS_CALM_FEEDBACK_GUARD_MS", 100.0) * 1000000.0);
  calm_cfg.pacing_floor_ns = (int64_t) (env_double (
    "CYCLONEDDS_CALM_PACING_MS",
    env_double ("CYCLONEDDS_CALM_PACING_FLOOR_MS", 50.0)) * 1000000.0);
  calm_cfg.pacing_drain_guard = env_double (
    "CYCLONEDDS_CALM_PACING_DRAIN_GUARD", 1.10);
  calm_cfg.decrease_gain = env_double ("CYCLONEDDS_CALM4_K_DEC", 0.25);
  calm_cfg.increase_gain = env_double ("CYCLONEDDS_CALM4_K_INC", 0.25);
  calm_cfg.min_budget_sample_multiplier = env_double (
    "CYCLONEDDS_CALM_MIN_BUDGET_SAMPLE_MULTIPLIER", 0.25);
  calm_cfg.initial_budget_sample_multiplier = env_double (
    "CYCLONEDDS_CALM_INITIAL_BUDGET_SAMPLE_MULTIPLIER", 1.0);
  calm_cfg.max_budget_sample_multiplier = env_double (
    "CYCLONEDDS_CALM_MAX_BUDGET_SAMPLE_MULTIPLIER", 2.0);
  calm_cfg.ai_budget_sample_multiplier = env_double (
    "CYCLONEDDS_CALM_AI_B_SAMPLE_MULTIPLIER", 0.25);
  calm_cfg.min_budget_floor = env_uint64 (
    "CYCLONEDDS_CALM_MIN_BUDGET_BYTES", UINT64_C (128) * 1024);
  calm_cfg.initial_budget_floor = env_uint64 (
    "CYCLONEDDS_CALM_INITIAL_BUDGET_BYTES", UINT64_C (512) * 1024);
  calm_cfg.max_budget_floor = env_uint64 (
    "CYCLONEDDS_CALM_MAX_BUDGET_BYTES", UINT64_C (2) * 1024 * 1024);
  calm_cfg.ai_budget_floor = env_uint64 (
    "CYCLONEDDS_CALM_AI_B_BYTES", UINT64_C (256) * 1024);
  calm_cfg.first_failure_gamma = env_double (
    "CYCLONEDDS_CALM_FIRST_FAILURE_GAMMA", 0.80);
  calm_cfg.repeated_failure_gamma = env_double (
    "CYCLONEDDS_CALM_REPEATED_FAILURE_GAMMA", 0.75);
  calm_cfg.first_failure_rate_gamma = env_double (
    "CYCLONEDDS_CALM_FIRST_FAILURE_RATE_GAMMA", 0.90);
  calm_cfg.repeated_failure_rate_gamma = env_double (
    "CYCLONEDDS_CALM_REPEATED_FAILURE_RATE_GAMMA", 0.875);
  calm_cfg.offered_alpha = env_double (
    "CYCLONEDDS_CALM_OFFERED_RATE_EWMA_ALPHA", 0.20);
  calm_cfg.service_alpha_up = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_ALPHA_UP", 0.25);
  calm_cfg.service_alpha_down = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_ALPHA_DOWN", 0.10);
  calm_cfg.service_failure_gamma = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_FAILURE_GAMMA", 0.98);
  calm_cfg.service_min_multiplier = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_MIN_MULTIPLIER", 0.25);
  calm_cfg.service_initial_multiplier = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_INITIAL_MULTIPLIER", 1.0);
  calm_cfg.service_max_multiplier = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_MAX_MULTIPLIER", 2.0);
  calm_cfg.service_ai_multiplier = env_double (
    "CYCLONEDDS_CALM_SERVICE_RATE_AI_MULTIPLIER", 0.25);
  {
    const char *feedback_progress =
      getenv ("CYCLONEDDS_CALM_FEEDBACK_BYTE_PROGRESS");
    calm_cfg.feedback_byte_progress = feedback_progress == NULL ||
      env_enabled (feedback_progress);
  }
  if (!storm_enabled)
    return;

  if (path == NULL || path[0] == '\0')
    path = "/tmp/cyclonedds_storm.csv";
  storm_log_file = fopen (path, "w");
  if (storm_log_file == NULL)
  {
    storm_enabled = false;
    return;
  }

  fputs (
    "time_ns,writer_guid,reader_guid,event,event_bytes,rho_bytes,oldest_repair_age_ms,"
    "requested_bytes,requested_changes,repair_unsent_bytes,repair_underway_bytes,"
    "repair_unacknowledged_bytes,nack_events_total,nack_bytes_total,"
    "unique_repair_bytes_total,repeated_nack_events_total,repeated_nack_bytes_total,"
    "tnr_release_events_total,tnr_requested_changes_total,tnr_requested_bytes_total,"
    "flow_enqueue_accepted_changes_total,flow_enqueue_accepted_bytes_total,"
    "flow_enqueue_rejected_changes_total,flow_enqueue_rejected_bytes_total,"
    "transport_repair_attempt_events_total,transport_repair_attempt_bytes_total,"
    "ack_recovered_bytes_total,calm_state,failed_feedback_rounds,repair_round,"
    "repair_round_armed,calm_window_bytes,release_credit,"
    "event_sample_seq,event_sample_retransmit_count,event_sample_failed_repair_count,"
    "oldest_repair_seq,"
    "oldest_repair_retransmit_count,oldest_no_progress_rounds,"
    "reader_acked_high_seq,oldest_repair_failed_repair_count,max_failed_repair_count,"
    "ack_progress_age_ms,nonshrinking_rounds,pending_repair_bytes,held_new_bytes,"
    "release_rate_mbps,service_rate_mbps,offered_rate_mbps,pacing_interval_ms,"
    "feedback_requested_bytes,feedback_retransmitted_scope_bytes,"
    "feedback_delivered_estimate_bytes,feedback_residual_ratio,failure_severity,"
    "effective_budget_gamma,effective_rate_gamma,"
    "feedback_progressing_changes,feedback_stalled_changes,"
    "calm4_delta_u_bytes,calm4_delta_oldest_failed,"
    "calm4_failure_fraction,calm4_progress_fraction,calm4_fixed_pacing_ms\n",
    storm_log_file);
  fflush (storm_log_file);
}

static bool bit_is_set (const uint8_t *bits, uint32_t index)
{
  return (bits[index / 8] & (uint8_t) (UINT8_C (1) << (index % 8))) != 0;
}

static void bit_set (uint8_t *bits, uint32_t index)
{
  bits[index / 8] |= (uint8_t) (UINT8_C (1) << (index % 8));
}

static void bit_clear (uint8_t *bits, uint32_t index)
{
  bits[index / 8] &= (uint8_t) ~(UINT8_C (1) << (index % 8));
}

static bool entry_has_pending (const struct ddsi_storm_repair_entry *entry)
{
  uint32_t i;
  for (i = 0; i < entry->nfrags; i++)
  {
    if (bit_is_set (entry->pending, i))
      return true;
  }
  return false;
}

static bool entry_has_outstanding (const struct ddsi_storm_repair_entry *entry)
{
  uint32_t i;
  for (i = 0; i < entry->nfrags; i++)
  {
    if (bit_is_set (entry->outstanding, i))
      return true;
  }
  return false;
}

static uint32_t fragment_bytes (const struct ddsi_storm_repair_entry *entry, uint32_t index)
{
  const uint64_t offset = (uint64_t) index * entry->fragment_size;
  if (offset >= entry->sample_size)
    return 0;
  if (entry->sample_size - offset < entry->fragment_size)
    return (uint32_t) (entry->sample_size - offset);
  return entry->fragment_size;
}

static struct ddsi_storm_repair_entry *find_repair (
  struct ddsi_storm_reader_state *state,
  seqno_t seq)
{
  struct ddsi_storm_repair_entry *entry;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    if (entry->seq == seq)
      return entry;
  }
  return NULL;
}

static struct ddsi_storm_repair_entry *oldest_repair (
  struct ddsi_storm_reader_state *state)
{
  struct ddsi_storm_repair_entry *entry;
  struct ddsi_storm_repair_entry *oldest = NULL;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    if (oldest == NULL || entry->seq < oldest->seq)
      oldest = entry;
  }
  return oldest;
}

static void note_batch_seq (
  struct ddsi_storm_nack_batch *batch,
  seqno_t seq)
{
  if (batch->single_seq == 0)
    batch->single_seq = seq;
  else if (batch->single_seq != seq)
    batch->multiple_sequences = true;
}

static struct ddsi_storm_repair_entry *get_or_create_repair (
  struct ddsi_storm_reader_state *state,
  seqno_t seq,
  uint32_t sample_size,
  uint32_t fragment_size)
{
  struct ddsi_storm_repair_entry *entry = find_repair (state, seq);
  if (entry != NULL)
    return entry;
  if (sample_size == 0 || fragment_size == 0)
    return NULL;

  entry = ddsrt_calloc (1, sizeof (*entry));
  entry->seq = seq;
  entry->sample_size = sample_size;
  entry->fragment_size = fragment_size;
  entry->nfrags = (uint32_t) (((uint64_t) sample_size + fragment_size - 1) / fragment_size);
  entry->outstanding = ddsrt_calloc ((entry->nfrags + 7) / 8, 1);
  entry->pending = ddsrt_calloc ((entry->nfrags + 7) / 8, 1);
  entry->sent_in_generation = ddsrt_calloc ((entry->nfrags + 7) / 8, 1);
  entry->first_nack_time = ddsrt_time_monotonic ();
  entry->next = state->repairs;
  state->repairs = entry;
  return entry;
}

static uint64_t oldest_repair_age_ns (const struct ddsi_storm_reader_state *state)
{
  const struct ddsi_storm_repair_entry *entry;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  uint64_t oldest = 0;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    const uint64_t age = now.v > entry->first_nack_time.v ?
      (uint64_t) (now.v - entry->first_nack_time.v) : 0;
    if (age > oldest)
      oldest = age;
  }
  return oldest;
}

static uint32_t max_failed_repair_count (const struct ddsi_storm_reader_state *state)
{
  const struct ddsi_storm_repair_entry *entry;
  uint32_t result = 0;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    if (entry->failed_repair_count > result)
      result = entry->failed_repair_count;
  }
  return result;
}

static uint64_t pending_repair_bytes (const struct ddsi_storm_reader_state *state)
{
  const struct ddsi_storm_repair_entry *entry;
  uint64_t result = 0;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    uint32_t i;
    for (i = 0; i < entry->nfrags; i++)
    {
      if (bit_is_set (entry->pending, i))
        result += fragment_bytes (entry, i);
    }
  }
  return result;
}

static uint64_t sent_bytes_in_feedback_scope (
  const struct ddsi_storm_repair_entry *entry,
  const struct ddsi_storm_nack_batch *batch)
{
  uint32_t begin = 0;
  uint32_t end = entry->nfrags;
  uint64_t bytes = 0;
  uint32_t i;
  if (batch->fragment_scope_valid)
  {
    begin = batch->fragment_scope_base < entry->nfrags ?
      batch->fragment_scope_base : entry->nfrags;
    end = batch->fragment_scope_numbits > entry->nfrags - begin ?
      entry->nfrags : begin + batch->fragment_scope_numbits;
  }
  for (i = begin; i < end; i++)
  {
    if (bit_is_set (entry->sent_in_generation, i))
      bytes += fragment_bytes (entry, i);
  }
  return bytes;
}

static bool have_pending_repair (const struct ddsi_storm_reader_state *state)
{
  const struct ddsi_storm_repair_entry *entry;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    if (entry_has_pending (entry))
      return true;
  }
  return false;
}

static void calm_refresh_budget_bounds (struct ddsi_storm_reader_state *state)
{
  const double sample = state->mean_sample_bytes > 0.0 ? state->mean_sample_bytes : 1.0;
  state->budget_min_bytes = max_u64 (
    calm_cfg.min_budget_floor,
    (uint64_t) ceil (calm_cfg.min_budget_sample_multiplier * sample));
  state->budget_max_bytes = max_u64 (
    calm_cfg.max_budget_floor,
    (uint64_t) ceil (calm_cfg.max_budget_sample_multiplier * sample));
  state->budget_ai_bytes = max_u64 (
    calm_cfg.ai_budget_floor,
    (uint64_t) ceil (calm_cfg.ai_budget_sample_multiplier * sample));
  if (state->budget_max_bytes < state->budget_min_bytes)
    state->budget_max_bytes = state->budget_min_bytes;
}

static int64_t calm_pacing_interval_ns (const struct ddsi_storm_reader_state *state)
{
  (void) state;
  return calm_cfg.pacing_floor_ns;
}

static void calm_schedule (struct ddsi_storm_reader_state *state, ddsrt_mtime_t now)
{
  if (state->pacing_event != NULL)
  {
    const ddsrt_mtime_t when = ddsrt_mtime_add_duration (
      now, calm_pacing_interval_ns (state));
    resched_xevent_if_earlier (state->pacing_event, when);
  }
}

static void calm_enter_active (struct ddsi_storm_reader_state *state, ddsrt_mtime_t now)
{
  const double sample = state->mean_sample_bytes > 0.0 ? state->mean_sample_bytes : 1.0;
  if (state->calm_active)
    return;
  calm_refresh_budget_bounds (state);
  state->budget_bytes = max_u64 (
    calm_cfg.initial_budget_floor,
    (uint64_t) ceil (calm_cfg.initial_budget_sample_multiplier * sample));
  if (state->budget_bytes < state->budget_min_bytes)
    state->budget_bytes = state->budget_min_bytes;
  if (state->budget_bytes > state->budget_max_bytes)
    state->budget_bytes = state->budget_max_bytes;
  state->release_credit = state->budget_bytes;
  state->calm_active = true;
  calm_schedule (state, now);
}

static void calm_apply_failure (
  struct ddsi_storm_reader_state *state,
  uint32_t delta_oldest_failed,
  int64_t delta_u,
  bool stalled,
  double severity)
{
  double effective_gamma_b;
  uint64_t reduced;
  if (severity < 0.0)
    severity = 0.0;
  else if (severity > 1.0)
    severity = 1.0;
  state->last_failure_severity = severity;
  state->last_effective_budget_gamma = 1.0;
  state->last_effective_rate_gamma = 1.0;
  if (delta_oldest_failed == 0 || (delta_u < 0 && !stalled) || severity == 0.0)
    return;
  if (!state->calm_active)
    calm_enter_active (state, ddsrt_time_monotonic ());
  effective_gamma_b = 1.0 - calm_cfg.decrease_gain * severity;
  if (effective_gamma_b < 0.0)
    effective_gamma_b = 0.0;
  state->last_effective_budget_gamma = effective_gamma_b;
  reduced = (uint64_t) floor (
    (double) state->budget_bytes * effective_gamma_b);
  state->budget_bytes = max_u64 (state->budget_min_bytes, reduced);
}

static void calm_note_repair_release (
  struct ddsi_storm_reader_state *state,
  uint64_t bytes,
  ddsrt_mtime_t now)
{
  const struct ddsi_storm_repair_entry *oldest;
  uint64_t room;

  if (!calm_cfg.enabled || bytes == 0)
    return;
  if (!state->repair_round_armed)
  {
    state->feedback_round_start_u_bytes = state->rho_bytes;
    state->feedback_round_released_bytes = 0;
    oldest = oldest_repair (state);
    state->feedback_round_start_oldest_failed_count =
      oldest == NULL ? 0 : oldest->failed_repair_count;
  }
  room = UINT64_MAX - state->feedback_round_released_bytes;
  state->feedback_round_released_bytes += bytes < room ? bytes : room;
  state->last_repair_time = now;
  state->repair_round_armed = true;
}

static void calm_pacing_cb (
  struct xevent *event,
  void *argument,
  ddsrt_mtime_t now);

static void log_snapshot (
  const struct ddsi_writer *wr,
  const struct ddsi_wr_prd_match *match,
  const char *event,
  uint64_t event_bytes,
  uint64_t requested_bytes,
  uint32_t requested_changes,
  uint64_t repair_unsent_bytes,
  uint64_t repair_underway_bytes,
  seqno_t event_sample_seq)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry *event_sample;
  struct ddsi_storm_repair_entry *oldest;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  uint64_t classified = repair_unsent_bytes + repair_underway_bytes;
  uint64_t repair_unacknowledged_bytes;
  uint64_t oldest_age_ns;

  if (state == NULL || !storm_enabled || storm_log_file == NULL)
    return;
  if (classified > state->rho_bytes)
    classified = state->rho_bytes;
  repair_unacknowledged_bytes = state->rho_bytes - classified;
  oldest_age_ns = oldest_repair_age_ns (state);
  event_sample = event_sample_seq == 0 ? NULL : find_repair (state, event_sample_seq);
  oldest = oldest_repair (state);
  state->pending_repair_bytes = pending_repair_bytes (state);

  ddsrt_mutex_lock (&storm_log_lock);
  fprintf (storm_log_file,
    "%" PRId64 "," PGUIDFMT "," PGUIDFMT ",%s,%" PRIu64 ",%" PRIu64 ",%.6f,"
    "%" PRIu64 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
    "%s,%" PRIu32 ",%" PRIu32 ",%u,%" PRIu64 ",%" PRIu64 ","
    "%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ","
    "%" PRIu32 ",%" PRIu32 ",%.6f,%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f,"
    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f,%" PRIu32 ",%" PRIu32 ","
    "%" PRId64 ",%" PRIu32 ",%.6f,%.6f,%.6f\n",
    ddsrt_time_wallclock ().v, PGUID (wr->e.guid), PGUID (match->prd_guid), event,
    event_bytes, state->rho_bytes, (double) oldest_age_ns / 1000000.0,
    requested_bytes, requested_changes, repair_unsent_bytes, repair_underway_bytes,
    repair_unacknowledged_bytes, state->nack_events_total, state->nack_bytes_total,
    state->unique_repair_bytes_total, state->repeated_nack_events_total,
    state->repeated_nack_bytes_total, state->schedule_events_total,
    state->schedule_requested_changes_total, state->schedule_requested_bytes_total,
    state->enqueue_accepted_changes_total, state->enqueue_accepted_bytes_total,
    state->enqueue_rejected_changes_total, state->enqueue_rejected_bytes_total,
    state->transport_repair_attempt_events_total,
    state->transport_repair_attempt_bytes_total, state->ack_recovered_bytes_total,
    state->calm_active ? "CALM_ACTIVE" : "NORMAL",
    oldest == NULL ? 0 : oldest->failed_repair_count,
    state->repair_round, state->repair_round_armed ? 1u : 0u,
    state->budget_bytes, state->release_credit,
    event_sample_seq, event_sample == NULL ? 0 : event_sample->retransmit_count,
    event_sample == NULL ? 0 : event_sample->failed_repair_count,
    oldest == NULL ? 0 : oldest->seq,
    oldest == NULL ? 0 : oldest->retransmit_count,
    state->oldest_no_progress_rounds, state->reader_acked_high_seq,
    oldest == NULL ? 0 : oldest->failed_repair_count,
    max_failed_repair_count (state),
    now.v > state->last_ack_progress_time.v ?
      (double) (now.v - state->last_ack_progress_time.v) / 1000000.0 : 0.0,
    state->nonshrinking_rounds, state->pending_repair_bytes,
    state->held_new_bytes, state->release_rate_bps / 1e6,
    state->service_rate_bps / 1e6, state->offered_rate_bps / 1e6,
    (double) calm_pacing_interval_ns (state) / 1000000.0,
    state->last_feedback_requested_bytes,
    state->last_feedback_previous_requested_bytes,
    state->last_feedback_decrease_bytes,
    state->last_feedback_residual_ratio,
    state->last_failure_severity,
    state->last_effective_budget_gamma,
    state->last_effective_rate_gamma,
    state->last_feedback_progressing_changes,
    state->last_feedback_stalled_changes,
    state->last_feedback_delta_u_bytes,
    state->last_feedback_delta_oldest_failed,
    state->last_failure_severity,
    state->last_feedback_progress_fraction,
    (double) calm_cfg.pacing_floor_ns / 1000000.0);
  if (++storm_rows_since_flush >= 64)
  {
    fflush (storm_log_file);
    storm_rows_since_flush = 0;
  }
  ddsrt_mutex_unlock (&storm_log_lock);
}

struct ddsi_storm_reader_state *ddsi_storm_reader_state_new (
  struct ddsi_writer *wr,
  const ddsi_guid_t *reader_guid,
  bool reliable)
{
  struct ddsi_storm_reader_state *state;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  ddsrt_once (&storm_once, storm_init);
  if ((!storm_enabled && !calm_cfg.enabled) || !reliable ||
      ddsi_is_builtin_entityid (wr->e.guid.entityid, NN_VENDORID_ECLIPSE))
    return NULL;
  state = ddsrt_calloc (1, sizeof (*state));
  state->gv = wr->e.gv;
  state->writer_guid = wr->e.guid;
  state->reader_guid = *reader_guid;
  state->last_write_time = now;
  state->last_ack_progress_time = now;
  state->last_service_sample_time = now;
  state->pacing_event = qxev_callback (
    wr->evq, DDSRT_MTIME_NEVER, calm_pacing_cb, state);
  return state;
}

void ddsi_storm_reader_state_free (struct ddsi_storm_reader_state *state)
{
  struct ddsi_storm_repair_entry *entry;
  struct ddsi_storm_held_new_entry *held;
  if (state == NULL)
    return;
  if (state->pacing_event != NULL)
    delete_xevent_callback (state->pacing_event);
  entry = state->repairs;
  while (entry != NULL)
  {
    struct ddsi_storm_repair_entry *next = entry->next;
    ddsrt_free (entry->outstanding);
    ddsrt_free (entry->pending);
    ddsrt_free (entry->sent_in_generation);
    ddsrt_free (entry);
    entry = next;
  }
  held = state->held_new;
  while (held != NULL)
  {
    struct ddsi_storm_held_new_entry *next = held->next;
    ddsrt_free (held);
    held = next;
  }
  if (storm_enabled && storm_log_file != NULL)
  {
    ddsrt_mutex_lock (&storm_log_lock);
    fflush (storm_log_file);
    storm_rows_since_flush = 0;
    ddsrt_mutex_unlock (&storm_log_lock);
  }
  ddsrt_free (state);
}

void ddsi_storm_nack_batch_init (struct ddsi_storm_nack_batch *batch)
{
  memset (batch, 0, sizeof (*batch));
}

void ddsi_storm_observe_ack (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  seqno_t ack_base)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry **cursor;
  uint64_t recovered = 0;
  const seqno_t acked_high = ack_base > 0 ? ack_base - 1 : 0;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  uint64_t acked_bytes = 0;
  if (state == NULL)
    return;

  if (acked_high > state->reader_acked_high_seq)
  {
    const uint64_t acked_samples = acked_high - state->reader_acked_high_seq;
    acked_bytes = (uint64_t) ceil (
      (double) acked_samples * state->mean_sample_bytes);
    state->reader_acked_high_seq = acked_high;
    state->oldest_no_progress_seq = 0;
    state->oldest_no_progress_rounds = 0;
    state->last_ack_progress_time = now;
    state->nonshrinking_rounds = 0;
    if (state->last_service_sample_time.v > 0 && now.v > state->last_service_sample_time.v && acked_bytes > 0)
    {
      const double elapsed = (double) (now.v - state->last_service_sample_time.v) / 1e9;
      const double raw = 8.0 * (double) acked_bytes / elapsed;
      const double cap_basis = max_double (state->release_rate_bps, state->offered_rate_bps);
      const double sample = cap_basis > 0.0 ?
        (raw < 1.05 * cap_basis ? raw : 1.05 * cap_basis) : raw;
      if (state->service_rate_bps <= 0.0)
        state->service_rate_bps = sample;
      else
      {
        const double alpha = sample >= state->service_rate_bps ?
          calm_cfg.service_alpha_up : calm_cfg.service_alpha_down;
        state->service_rate_bps =
          (1.0 - alpha) * state->service_rate_bps + alpha * sample;
      }
      state->last_service_sample_time = now;
    }
  }

  cursor = &state->repairs;
  while (*cursor != NULL)
  {
    struct ddsi_storm_repair_entry *entry = *cursor;
    if (entry->seq < ack_base)
    {
      if (entry_has_outstanding (entry))
        recovered += entry->sample_size;
      *cursor = entry->next;
      ddsrt_free (entry->outstanding);
      ddsrt_free (entry->pending);
      ddsrt_free (entry->sent_in_generation);
      ddsrt_free (entry);
    }
    else
    {
      cursor = &entry->next;
    }
  }

  if (recovered > state->rho_bytes)
    state->rho_bytes = 0;
  else
    state->rho_bytes -= recovered;
  state->ack_recovered_bytes_total += recovered;
  if (recovered > 0 && state->repair_round_armed)
  {
    const int64_t delta_u = state->rho_bytes >= state->feedback_round_start_u_bytes ?
      (int64_t) (state->rho_bytes - state->feedback_round_start_u_bytes) :
      -(int64_t) (state->feedback_round_start_u_bytes - state->rho_bytes);
    const uint64_t reference = state->feedback_round_released_bytes > 0 ?
      state->feedback_round_released_bytes : 1;
    double progress = (double) recovered / (double) reference;
    uint64_t increase;
    if (progress > 1.0)
      progress = 1.0;
    if (state->calm_active)
    {
      calm_refresh_budget_bounds (state);
      if (delta_u < 0)
      {
        increase = (uint64_t) ceil (
          calm_cfg.increase_gain * state->mean_sample_bytes * progress);
        if (state->budget_max_bytes - state->budget_bytes < increase)
          state->budget_bytes = state->budget_max_bytes;
        else
          state->budget_bytes += increase;
      }
    }
    state->last_feedback_delta_u_bytes = delta_u;
    state->last_feedback_delta_oldest_failed = 0;
    state->last_feedback_progress_fraction = progress;
    state->last_failure_severity = 1.0 - progress;
    state->repair_round_armed = false;
    state->feedback_round_start_u_bytes = state->rho_bytes;
    state->feedback_round_released_bytes = 0;
    {
      const struct ddsi_storm_repair_entry *oldest = oldest_repair (state);
      state->feedback_round_start_oldest_failed_count =
        oldest == NULL ? 0 : oldest->failed_repair_count;
    }
  }
  if (state->rho_bytes == 0 && state->held_new == NULL)
    state->calm_active = false;
  else if (state->calm_active)
    calm_schedule (state, now);
  if (acked_bytes > 0 || recovered > 0)
    log_snapshot (wr, match, "ack", recovered, 0, 0, 0, 0, 0);
}

void ddsi_storm_observe_sample_nack (
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint32_t sample_size,
  uint32_t fragment_size,
  struct ddsi_storm_nack_batch *batch)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry *entry;
  bool was_outstanding;
  uint32_t i;
  if (state == NULL)
    return;
  entry = get_or_create_repair (state, seq, sample_size, fragment_size);
  if (entry == NULL)
    return;
  was_outstanding = entry_has_outstanding (entry);

  note_batch_seq (batch, seq);

  batch->requested_changes++;
  batch->requested_bytes += entry->sample_size;
  entry->feedback_requested_bytes += entry->sample_size;
  for (i = 0; i < entry->nfrags; i++)
  {
    const uint64_t bytes = fragment_bytes (entry, i);
    if (bit_is_set (entry->outstanding, i))
    {
      batch->repeated_bytes += bytes;
      if (bit_is_set (entry->sent_in_generation, i))
        entry->feedback_rerequested_sent_bytes += bytes;
    }
    else
    {
      bit_set (entry->outstanding, i);
      batch->unique_bytes += bytes;
    }
    bit_set (entry->pending, i);
  }
  if (!was_outstanding && entry_has_outstanding (entry))
    state->rho_bytes += entry->sample_size;
  entry->nack_seen = true;
}

void ddsi_storm_observe_fragment_nack (
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint32_t sample_size,
  uint32_t fragment_size,
  uint32_t fragment_index,
  struct ddsi_storm_nack_batch *batch)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry *entry;
  bool was_outstanding;
  uint64_t bytes;
  if (state == NULL)
    return;
  entry = get_or_create_repair (state, seq, sample_size, fragment_size);
  if (entry == NULL || fragment_index >= entry->nfrags)
    return;
  was_outstanding = entry_has_outstanding (entry);

  note_batch_seq (batch, seq);

  bytes = fragment_bytes (entry, fragment_index);
  batch->requested_bytes += bytes;
  entry->feedback_requested_bytes += bytes;
  if (bit_is_set (entry->outstanding, fragment_index))
  {
    batch->repeated_bytes += bytes;
    if (bit_is_set (entry->sent_in_generation, fragment_index))
      entry->feedback_rerequested_sent_bytes += bytes;
  }
  else
  {
    bit_set (entry->outstanding, fragment_index);
    batch->unique_bytes += bytes;
  }
  bit_set (entry->pending, fragment_index);
  if (!was_outstanding)
    state->rho_bytes += entry->sample_size;
  entry->nack_seen = true;
}

void ddsi_storm_observe_nack_batch (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  const char *event,
  struct ddsi_storm_nack_batch *batch)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry *entry;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  uint64_t comparable_previous_bytes = 0;
  uint64_t comparable_current_bytes = 0;
  uint64_t decrease_bytes = 0;
  uint32_t progressing_changes = 0;
  uint32_t stalled_changes = 0;
  double failure_severity = 1.0;
  uint64_t unsent;
  if (state == NULL || batch->requested_bytes == 0)
    return;
  state->nack_events_total++;
  state->nack_bytes_total += batch->requested_bytes;
  state->unique_repair_bytes_total += batch->unique_bytes;
  if (batch->repeated_bytes > 0)
  {
    state->repeated_nack_events_total++;
    state->repeated_nack_bytes_total += batch->repeated_bytes;
  }
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    if (entry->nack_seen)
    {
      if (entry->sent_generation > entry->failed_generation &&
          now.v >= entry->generation_first_retransmit_time.v + calm_cfg.feedback_guard_ns)
      {
        const uint64_t retransmitted_scope_bytes =
          sent_bytes_in_feedback_scope (entry, batch);
        const bool evaluate = !calm_cfg.feedback_byte_progress ||
          retransmitted_scope_bytes > 0;
        if (evaluate)
        {
          const uint64_t reference = calm_cfg.feedback_byte_progress ?
            retransmitted_scope_bytes : entry->feedback_requested_bytes;
          const uint64_t rerequested = calm_cfg.feedback_byte_progress ?
            (entry->feedback_rerequested_sent_bytes < reference ?
             entry->feedback_rerequested_sent_bytes : reference) : reference;
          entry->failed_generation = entry->sent_generation;
          if (reference > 0 && rerequested > 0)
          {
            entry->failed_repair_count++;
            if (entry->failed_repair_count > batch->max_failed_repair_count)
              batch->max_failed_repair_count = entry->failed_repair_count;
          }
          comparable_previous_bytes += reference;
          comparable_current_bytes += rerequested;
          if (rerequested < reference)
          {
            decrease_bytes += reference - rerequested;
            progressing_changes++;
          }
          else
            stalled_changes++;
        }
      }
      entry->request_generation++;
      entry->previous_feedback_requested_bytes = entry->feedback_requested_bytes;
      entry->previous_feedback_requested_valid = true;
      entry->feedback_requested_bytes = 0;
      entry->feedback_rerequested_sent_bytes = 0;
      entry->nack_seen = false;
    }
  }
  if (calm_cfg.feedback_byte_progress)
  {
    failure_severity = comparable_previous_bytes > 0 ?
      (double) comparable_current_bytes /
      (double) comparable_previous_bytes : 0.0;
  }
  state->last_feedback_requested_bytes = batch->requested_bytes;
  state->last_feedback_previous_requested_bytes = comparable_previous_bytes;
  state->last_feedback_decrease_bytes = decrease_bytes;
  state->last_feedback_residual_ratio = comparable_previous_bytes > 0 ?
    (double) comparable_current_bytes / (double) comparable_previous_bytes : 1.0;
  state->last_feedback_progressing_changes = progressing_changes;
  state->last_feedback_stalled_changes = stalled_changes;
  if (state->repair_round_armed &&
      now.v >= state->last_repair_time.v + calm_cfg.feedback_guard_ns)
  {
    const struct ddsi_storm_repair_entry *oldest = oldest_repair (state);
    const uint32_t current_oldest_failed =
      oldest == NULL ? 0 : oldest->failed_repair_count;
    const uint32_t delta_oldest_failed =
      current_oldest_failed >= state->feedback_round_start_oldest_failed_count ?
      current_oldest_failed - state->feedback_round_start_oldest_failed_count : 0;
    const int64_t delta_u = state->rho_bytes >= state->feedback_round_start_u_bytes ?
      (int64_t) (state->rho_bytes - state->feedback_round_start_u_bytes) :
      -(int64_t) (state->feedback_round_start_u_bytes - state->rho_bytes);
    const int64_t ack_age_ns = now.v > state->last_ack_progress_time.v ?
      now.v - state->last_ack_progress_time.v : 0;
    const bool stalled = ack_age_ns >= calm_cfg.ack_stall_ns &&
      failure_severity >= 1.0;

    state->repair_round++;
    if (state->rho_bytes >= state->previous_feedback_rho_bytes)
      state->nonshrinking_rounds++;
    else
      state->nonshrinking_rounds = 0;
    state->previous_feedback_rho_bytes = state->rho_bytes;
    state->last_feedback_delta_u_bytes = delta_u;
    state->last_feedback_delta_oldest_failed = delta_oldest_failed;
    state->last_feedback_progress_fraction = 1.0 - failure_severity;
    calm_apply_failure (
      state, delta_oldest_failed, delta_u, stalled, failure_severity);
    state->repair_round_armed = false;
    state->feedback_round_start_u_bytes = state->rho_bytes;
    state->feedback_round_released_bytes = 0;
    state->feedback_round_start_oldest_failed_count = current_oldest_failed;
  }
  state->pending_repair_bytes = pending_repair_bytes (state);
  if (state->calm_active)
    calm_schedule (state, now);
  unsent = batch->requested_bytes < state->rho_bytes ?
    batch->requested_bytes : state->rho_bytes;
  log_snapshot (wr, match, event, batch->requested_bytes,
    batch->requested_bytes, batch->requested_changes, unsent, 0,
    batch->multiple_sequences ? 0 : batch->single_seq);
}

void ddsi_storm_observe_rexmit_schedule (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  const struct ddsi_storm_nack_batch *batch,
  uint64_t accepted_bytes,
  uint32_t accepted_changes,
  uint64_t rejected_bytes,
  uint32_t rejected_changes)
{
  struct ddsi_storm_reader_state *state = match->storm;
  if (state == NULL || (batch->requested_bytes == 0 && accepted_bytes == 0 && rejected_bytes == 0))
    return;
  state->schedule_events_total++;
  state->schedule_requested_changes_total += batch->requested_changes;
  state->schedule_requested_bytes_total += batch->requested_bytes;
  state->enqueue_accepted_changes_total += accepted_changes;
  state->enqueue_accepted_bytes_total += accepted_bytes;
  state->enqueue_rejected_changes_total += rejected_changes;
  state->enqueue_rejected_bytes_total += rejected_bytes;
  state->transport_repair_attempt_events_total += accepted_changes;
  state->transport_repair_attempt_bytes_total += accepted_bytes;
  log_snapshot (wr, match, "rexmit_schedule", accepted_bytes,
    batch->requested_bytes, batch->requested_changes, 0, accepted_bytes,
    batch->multiple_sequences ? 0 : batch->single_seq);
}

void ddsi_storm_observe_sample_retransmit (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint64_t bytes)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry *entry;
  struct ddsi_storm_repair_entry *oldest;
  bool generation_started = false;
  if (state == NULL || bytes == 0)
    return;

  entry = find_repair (state, seq);
  if (entry == NULL)
    return;

  if (entry->sent_generation != entry->request_generation)
  {
    memset (entry->sent_in_generation, 0, (entry->nfrags + 7) / 8);
    entry->retransmit_count++;
    entry->sent_generation = entry->request_generation;
    entry->generation_first_retransmit_time = ddsrt_time_monotonic ();
    generation_started = true;
  }
  if (generation_started && entry->nfrags > 0)
    bit_set (entry->sent_in_generation, 0);
  entry->last_retransmit_time = ddsrt_time_monotonic ();
  calm_note_repair_release (state, bytes, entry->last_retransmit_time);
  oldest = oldest_repair (state);
  if (oldest != NULL && oldest->seq == seq)
  {
    if (state->oldest_no_progress_seq != seq)
    {
      state->oldest_no_progress_seq = seq;
      state->oldest_no_progress_rounds = 0;
    }
    state->oldest_no_progress_rounds++;
  }

  log_snapshot (wr, match, "sample_retransmit", bytes, 0, 0, 0, bytes, seq);
}

void ddsi_storm_observe_fragment_retransmit (
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint32_t fragment_index)
{
  struct ddsi_storm_reader_state *state = match->storm;
  struct ddsi_storm_repair_entry *entry;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  if (state == NULL)
    return;
  entry = find_repair (state, seq);
  if (entry == NULL || fragment_index >= entry->nfrags)
    return;
  if (entry->sent_generation != entry->request_generation)
  {
    memset (entry->sent_in_generation, 0, (entry->nfrags + 7) / 8);
    entry->retransmit_count++;
    entry->sent_generation = entry->request_generation;
    entry->generation_first_retransmit_time = now;
  }
  bit_set (entry->sent_in_generation, fragment_index);
  entry->last_retransmit_time = now;
}

bool ddsi_storm_calm_defer_retransmit (
  const struct ddsi_wr_prd_match *match)
{
  return match != NULL && match->storm != NULL && match->storm->calm_active;
}

static struct ddsi_storm_repair_entry *oldest_pending_repair (
  struct ddsi_storm_reader_state *state)
{
  struct ddsi_storm_repair_entry *entry;
  struct ddsi_storm_repair_entry *oldest = NULL;
  for (entry = state->repairs; entry != NULL; entry = entry->next)
  {
    if (entry_has_pending (entry) && (oldest == NULL || entry->seq < oldest->seq))
      oldest = entry;
  }
  return oldest;
}

static void append_held_new (
  struct ddsi_storm_reader_state *state,
  seqno_t seq,
  uint32_t sample_size)
{
  struct ddsi_storm_held_new_entry **cursor = &state->held_new;
  struct ddsi_storm_held_new_entry *entry;
  while (*cursor != NULL)
  {
    if ((*cursor)->seq == seq)
      return;
    cursor = &(*cursor)->next;
  }
  entry = ddsrt_calloc (1, sizeof (*entry));
  entry->seq = seq;
  entry->sample_size = sample_size;
  *cursor = entry;
  state->held_new_bytes += sample_size;
}

bool ddsi_storm_observe_new_sample (
  struct ddsi_writer *wr,
  seqno_t seq,
  uint32_t sample_size)
{
  ddsrt_avl_iter_t iterator;
  struct ddsi_wr_prd_match *match;
  struct ddsi_wr_prd_match *owner = NULL;
  const ddsrt_mtime_t now = ddsrt_time_monotonic ();
  for (match = ddsrt_avl_iter_first (
         &ddsi_wr_readers_treedef, &wr->readers, &iterator);
       match != NULL;
       match = ddsrt_avl_iter_next (&iterator))
  {
    struct ddsi_storm_reader_state *state = match->storm;
    if (state == NULL)
      continue;
    state->observed_sample_bytes += sample_size;
    state->observed_sample_count++;
    state->mean_sample_bytes = (double) state->observed_sample_bytes /
      (double) state->observed_sample_count;
    if (state->last_write_time.v > 0 && now.v > state->last_write_time.v)
    {
      const double instantaneous = 8.0 * (double) sample_size * 1e9 /
        (double) (now.v - state->last_write_time.v);
      if (state->offered_rate_bps <= 0.0)
        state->offered_rate_bps = instantaneous;
      else
        state->offered_rate_bps =
          (1.0 - calm_cfg.offered_alpha) * state->offered_rate_bps +
          calm_cfg.offered_alpha * instantaneous;
    }
    state->last_write_time = now;
    state->offered_window_bytes += sample_size;
    if (owner == NULL && state->calm_active)
      owner = match;
  }
  if (owner == NULL)
    return false;
  append_held_new (owner->storm, seq, sample_size);
  calm_schedule (owner->storm, now);
  log_snapshot (wr, owner, "held_new", sample_size, 0, 0, 0, 0, seq);
  return true;
}

static void calm_note_paced_repair (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  struct ddsi_storm_repair_entry *entry,
  uint32_t fragment_index,
  uint64_t bytes,
  ddsrt_mtime_t now)
{
  struct ddsi_storm_reader_state *state = match->storm;
  if (entry->sent_generation != entry->request_generation)
  {
    memset (entry->sent_in_generation, 0, (entry->nfrags + 7) / 8);
    entry->retransmit_count++;
    entry->sent_generation = entry->request_generation;
    entry->generation_first_retransmit_time = now;
  }
  bit_set (entry->sent_in_generation, fragment_index);
  entry->last_retransmit_time = now;
  calm_note_repair_release (state, bytes, now);
  state->transport_repair_attempt_events_total++;
  state->transport_repair_attempt_bytes_total += bytes;
  state->enqueue_accepted_changes_total++;
  state->enqueue_accepted_bytes_total += bytes;
  log_snapshot (wr, match, "calm_repair_release", bytes, 0, 0, 0, bytes, entry->seq);
}

static void calm_pacing_cb (
  struct xevent *event,
  void *argument,
  ddsrt_mtime_t now)
{
  struct ddsi_storm_reader_state *state = argument;
  struct ddsi_writer *wr;
  struct ddsi_proxy_reader *prd;
  struct ddsi_wr_prd_match *match;
  uint64_t released = 0;
  uint32_t released_changes = 0;
  bool work_remains = false;
  (void) event;

  if (now.v == DDS_NEVER || !state->calm_active)
    return;
  wr = entidx_lookup_writer_guid (state->gv->entity_index, &state->writer_guid);
  prd = entidx_lookup_proxy_reader_guid (state->gv->entity_index, &state->reader_guid);
  if (wr == NULL || prd == NULL)
    return;

  ddsrt_mutex_lock (&wr->e.lock);
  match = ddsrt_avl_lookup (
    &ddsi_wr_readers_treedef, &wr->readers, &state->reader_guid);
  if (match == NULL || match->storm != state || !state->calm_active)
  {
    ddsrt_mutex_unlock (&wr->e.lock);
    return;
  }

  if (qxev_queued_rexmit_bytes (wr->evq) == 0)
  {
    while (released < state->budget_bytes)
    {
      struct ddsi_storm_repair_entry *entry = oldest_pending_repair (state);
      struct whc_borrowed_sample sample;
      uint32_t fragment_index;
      uint32_t bytes;
      struct nn_xmsg *message;
      enum qxev_msg_rexmit_result result;
      if (entry == NULL)
        break;
      for (fragment_index = 0; fragment_index < entry->nfrags; fragment_index++)
      {
        if (bit_is_set (entry->pending, fragment_index))
          break;
      }
      if (fragment_index == entry->nfrags)
        break;
      bytes = fragment_bytes (entry, fragment_index);
      if (released > 0 && released + bytes > state->budget_bytes)
        break;
      if (!whc_borrow_sample (wr->whc, entry->seq, &sample))
      {
        bit_clear (entry->pending, fragment_index);
        continue;
      }
      message = NULL;
      if (create_fragment_message (
            wr, entry->seq, sample.plist, sample.serdata, fragment_index, 1,
            prd, &message, 0, 0) < 0)
      {
        whc_return_sample (wr->whc, &sample, false);
        break;
      }
      result = qxev_msg_rexmit_wrlock_held (wr->evq, message, 0);
      whc_return_sample (wr->whc, &sample, false);
      if (result == QXEV_MSG_REXMIT_DROPPED)
        break;
      bit_clear (entry->pending, fragment_index);
      released += bytes;
      released_changes++;
      wr->rexmit_bytes += bytes;
      wr->rexmit_count++;
      calm_note_paced_repair (
        wr, match, entry, fragment_index, bytes, now);
    }
  }

  if (!have_pending_repair (state))
  {
    while (state->held_new != NULL && released < state->budget_bytes)
    {
      struct ddsi_storm_held_new_entry *held = state->held_new;
      struct whc_borrowed_sample sample;
      const uint32_t nfrags = (held->sample_size + state->gv->config.fragment_size - 1) /
        state->gv->config.fragment_size;
      const uint64_t offset = (uint64_t) held->next_fragment * state->gv->config.fragment_size;
      const uint32_t bytes = held->sample_size - offset < state->gv->config.fragment_size ?
        (uint32_t) (held->sample_size - offset) : state->gv->config.fragment_size;
      struct nn_xmsg *message = NULL;
      if (released > 0 && released + bytes > state->budget_bytes)
        break;
      if (!whc_borrow_sample (wr->whc, held->seq, &sample))
      {
        state->held_new = held->next;
        state->held_new_bytes = held->sample_size > state->held_new_bytes ?
          0 : state->held_new_bytes - held->sample_size;
        ddsrt_free (held);
        continue;
      }
      if (create_fragment_message (
            wr, held->seq, sample.plist, sample.serdata, held->next_fragment, 1,
            NULL, &message, 1,
            held->next_fragment + 1 == nfrags ? held->next_fragment : UINT32_MAX) < 0)
      {
        whc_return_sample (wr->whc, &sample, false);
        break;
      }
      qxev_msg (wr->evq, message);
      whc_return_sample (wr->whc, &sample, false);
      held->next_fragment++;
      released += bytes;
      released_changes++;
      if (held->next_fragment >= nfrags)
      {
        ddsi_writer_update_seq_xmit (wr, held->seq);
        state->held_new = held->next;
        state->held_new_bytes = held->sample_size > state->held_new_bytes ?
          0 : state->held_new_bytes - held->sample_size;
        ddsrt_free (held);
      }
    }
  }

  state->last_release_bytes = released;
  state->release_credit = released >= state->budget_bytes ?
    0 : state->budget_bytes - released;
  state->pending_repair_bytes = pending_repair_bytes (state);
  if (released > 0)
  {
    writer_hbcontrol_note_asyncwrite (wr, now);
    log_snapshot (wr, match, "calm_pacing_tick", released, 0,
      released_changes, state->pending_repair_bytes, released, 0);
  }
  if (state->rho_bytes == 0 && state->held_new == NULL)
    state->calm_active = false;
  work_remains = have_pending_repair (state) || state->held_new != NULL;
  ddsrt_mutex_unlock (&wr->e.lock);

  if (state->calm_active && work_remains)
    calm_schedule (state, now);
}
