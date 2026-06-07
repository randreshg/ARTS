/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

/// @file db_move.h
/// @brief Lazy DB-move granularity policy over committed runtime facts.
///
/// Pure policy: choose local, slice, whole-DB, or reject from ownership,
/// residency, access mode, and the committed dependency byte footprint.

#ifndef ARTS_MEMORY_DB_MOVE_H
#define ARTS_MEMORY_DB_MOVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Movement granularity chosen for one dependency slot.
typedef enum arts_db_move_kind {
  ARTS_DB_MOVE_LOCAL = 0, ///< already resident — no transfer
  ARTS_DB_MOVE_SLICE,     ///< demand-move only the committed footprint
  ARTS_DB_MOVE_WHOLE,     ///< conservative whole-DB transfer
  ARTS_DB_MOVE_REJECT,    ///< fail closed: a footprint is present but unusable
} arts_db_move_kind_t;

/// Why the granularity was chosen.
typedef enum arts_db_move_reason {
  ARTS_DB_MOVE_REASON_NONE = 0,
  ARTS_DB_MOVE_REASON_OWNER_LOCAL,   ///< local: this rank owns the block
  ARTS_DB_MOVE_REASON_CACHE_VALID,   ///< local: a valid local copy is held
  ARTS_DB_MOVE_REASON_FOOTPRINT,     ///< slice: in-bounds read-only footprint
  ARTS_DB_MOVE_REASON_NO_FOOTPRINT,  ///< whole: no committed footprint
  ARTS_DB_MOVE_REASON_WRITE_MODE,    ///< whole: writer keeps the whole block
  ARTS_DB_MOVE_REASON_WRITE_FOOTPRINT,      ///< reject: footprint on a writer
  ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS, ///< reject: slice exceeds extent
} arts_db_move_reason_t;

/// A granularity decision plus its reason.
typedef struct arts_db_move_plan_s {
  arts_db_move_kind_t kind;
  arts_db_move_reason_t reason;
} arts_db_move_plan_t;

/// Decide the movement granularity for one dependency slot.
///
/// @param read_only             access mode is DB_MODE_RO.
/// @param owner_is_local        the block's owner rank is this rank.
/// @param local_copy_valid      a valid whole-DB copy is already resident here
///                              and no transfer is needed.
/// @param footprint_offset      committed reader footprint byte offset.
/// @param footprint_size        committed reader footprint byte length; 0 means
///                              no footprint was committed (whole DB).
/// @param resident_payload_size locally-known DB payload extent in bytes, or 0
///                              when the holder must validate the extent.
///
/// Precedence: a locally resident block never moves; otherwise a missing
/// footprint or any write mode keeps the whole-DB path; a footprint on a writer
/// or one that exceeds a known extent is rejected; an in-bounds read-only
/// footprint moves only its bytes.
arts_db_move_plan_t arts_db_move_plan(bool read_only, bool owner_is_local,
                                      bool local_copy_valid,
                                      uint64_t footprint_offset,
                                      uint64_t footprint_size,
                                      uint64_t resident_payload_size);

/// Human-readable name for a granularity kind.
const char *arts_db_move_kind_str(arts_db_move_kind_t kind);

/// Human-readable name for a reason.
const char *arts_db_move_reason_str(arts_db_move_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_MEMORY_DB_MOVE_H */
