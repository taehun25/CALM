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
#ifndef DDSI_STORM_OBSERVER_H
#define DDSI_STORM_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "dds/ddsi/ddsi_guid.h"
#include "dds/ddsi/q_rtps.h"

#if defined (__cplusplus)
extern "C" {
#endif

struct ddsi_writer;
struct ddsi_wr_prd_match;
struct ddsi_storm_reader_state;

struct ddsi_storm_nack_batch
{
  uint64_t requested_bytes;
  uint64_t unique_bytes;
  uint64_t repeated_bytes;
  uint32_t requested_changes;
  seqno_t single_seq;
  bool multiple_sequences;
  uint32_t max_failed_repair_count;
  bool fragment_scope_valid;
  uint32_t fragment_scope_base;
  uint32_t fragment_scope_numbits;
};

struct ddsi_storm_reader_state *ddsi_storm_reader_state_new (
  struct ddsi_writer *wr,
  const ddsi_guid_t *reader_guid,
  bool reliable);

void ddsi_storm_reader_state_free (
  struct ddsi_storm_reader_state *state);

void ddsi_storm_nack_batch_init (
  struct ddsi_storm_nack_batch *batch);

void ddsi_storm_observe_ack (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  seqno_t ack_base);

void ddsi_storm_observe_sample_nack (
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint32_t sample_size,
  uint32_t fragment_size,
  struct ddsi_storm_nack_batch *batch);

void ddsi_storm_observe_fragment_nack (
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint32_t sample_size,
  uint32_t fragment_size,
  uint32_t fragment_index,
  struct ddsi_storm_nack_batch *batch);

void ddsi_storm_observe_nack_batch (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  const char *event,
  struct ddsi_storm_nack_batch *batch);

void ddsi_storm_observe_rexmit_schedule (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  const struct ddsi_storm_nack_batch *batch,
  uint64_t accepted_bytes,
  uint32_t accepted_changes,
  uint64_t rejected_bytes,
  uint32_t rejected_changes);

void ddsi_storm_observe_sample_retransmit (
  struct ddsi_writer *wr,
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint64_t bytes);

void ddsi_storm_observe_fragment_retransmit (
  struct ddsi_wr_prd_match *match,
  seqno_t seq,
  uint32_t fragment_index);

bool ddsi_storm_calm_defer_retransmit (
  const struct ddsi_wr_prd_match *match);

bool ddsi_storm_observe_new_sample (
  struct ddsi_writer *wr,
  seqno_t seq,
  uint32_t sample_size);

#if defined (__cplusplus)
}
#endif

#endif /* DDSI_STORM_OBSERVER_H */
