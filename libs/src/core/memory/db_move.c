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

#include "arts/memory/db_move.h"

arts_db_move_plan_t arts_db_move_plan(bool read_only, bool owner_is_local,
                                      bool local_copy_valid,
                                      uint64_t footprint_offset,
                                      uint64_t footprint_size,
                                      uint64_t resident_payload_size) {
  arts_db_move_plan_t plan;

  /* Residency wins: never transfer data that is already valid locally. */
  if (owner_is_local) {
    plan.kind = ARTS_DB_MOVE_LOCAL;
    plan.reason = ARTS_DB_MOVE_REASON_OWNER_LOCAL;
    return plan;
  }
  if (local_copy_valid) {
    plan.kind = ARTS_DB_MOVE_LOCAL;
    plan.reason = ARTS_DB_MOVE_REASON_CACHE_VALID;
    return plan;
  }

  bool has_footprint = (footprint_size != 0);

  /* Missing footprint or write mode stays at whole-DB granularity. */
  if (!has_footprint) {
    plan.kind = ARTS_DB_MOVE_WHOLE;
    plan.reason = read_only ? ARTS_DB_MOVE_REASON_NO_FOOTPRINT
                            : ARTS_DB_MOVE_REASON_WRITE_MODE;
    return plan;
  }
  if (!read_only) {
    /* A writer footprint could drop bytes, so reject it. */
    plan.kind = ARTS_DB_MOVE_REJECT;
    plan.reason = ARTS_DB_MOVE_REASON_WRITE_FOOTPRINT;
    return plan;
  }

  /* Validate local extents; remote holders re-validate unknown extents. */
  if (resident_payload_size != 0 &&
      (footprint_offset > resident_payload_size ||
       footprint_size > resident_payload_size - footprint_offset)) {
    plan.kind = ARTS_DB_MOVE_REJECT;
    plan.reason = ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS;
    return plan;
  }

  plan.kind = ARTS_DB_MOVE_SLICE;
  plan.reason = ARTS_DB_MOVE_REASON_FOOTPRINT;
  return plan;
}

/* Default-less switches let -Wswitch catch new enumerators. */

const char *arts_db_move_kind_str(arts_db_move_kind_t kind) {
  switch (kind) {
  case ARTS_DB_MOVE_LOCAL:
    return "local";
  case ARTS_DB_MOVE_SLICE:
    return "slice";
  case ARTS_DB_MOVE_WHOLE:
    return "whole";
  case ARTS_DB_MOVE_REJECT:
    return "reject";
  }
  return "unknown kind";
}

const char *arts_db_move_reason_str(arts_db_move_reason_t reason) {
  switch (reason) {
  case ARTS_DB_MOVE_REASON_NONE:
    return "none";
  case ARTS_DB_MOVE_REASON_OWNER_LOCAL:
    return "owner-local";
  case ARTS_DB_MOVE_REASON_CACHE_VALID:
    return "cache-valid";
  case ARTS_DB_MOVE_REASON_FOOTPRINT:
    return "committed-footprint";
  case ARTS_DB_MOVE_REASON_NO_FOOTPRINT:
    return "no-footprint";
  case ARTS_DB_MOVE_REASON_WRITE_MODE:
    return "write-mode-whole";
  case ARTS_DB_MOVE_REASON_WRITE_FOOTPRINT:
    return "write-footprint-unsafe";
  case ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS:
    return "footprint-out-of-bounds";
  }
  return "unknown reason";
}
