/* SPDX-License-Identifier: Apache-2.0
 *
 * The install primitive of the route-table and out-of-order units: install a
 * control block made by arts_route_table_make_handle under `key` if the key's
 * slot is empty, as arts_route_table_install_or_get does for a creating
 * thread, and say whether it won.
 *
 * The caller holds a ref of its own on `cb` across the call.  On a win the
 * slot takes over the ref the caller passed, the cb's home is recorded and the
 * slot's OoO list drained on it, and the caller's own ref is the one it still
 * holds; on a loss the slot held `key`'s object and `cb` stays unpublished,
 * the caller's to abandon with its object.  The ref install_or_get returns (the
 * cb on a win, the occupant's pin on a loss) is dropped before the return, so
 * the caller's counts are those of the call alone.
 */
#ifndef ROUTE_INSTALL_TEST_H
#define ROUTE_INSTALL_TEST_H

#include <stdbool.h>

static bool install_if_absent(arts_shared_ptr_t cb, arts_guid_t key) {
  arts_route_item_t *slot;
  arts_route_table_reserve_or_lookup(key, &slot);
  arts_shared_ptr_t h = arts_route_table_install_or_get(slot, cb, key);
  bool won = h == cb;
  arts_shared_release(&h);
  return won;
}

#endif
