/* SPDX-License-Identifier: Apache-2.0
 *
 * The pairs the dependence entries refuse.  Each entry learns what its source
 * and destination are from their GUIDs on the calling rank, and a pair it
 * cannot serve is a program error that ends the run with a fatal error naming
 * both GUIDs (ARTS_ERROR: exit status 1) before anything is satisfied,
 * registered or sent:
 *   - arts_add_dependence, the polymorphic entry: a destination that is
 *     neither an EDT nor an event, whatever the source, or a source that is
 *     neither NULL_GUID, a data block nor an event;
 *   - arts_event_add_dependence_to_edt: a source that is not an event, or a
 *     destination that is not an EDT;
 *   - arts_event_add_dependence_to_event: a source or a destination that is
 *     not an event.
 *
 * argv[1] names the entry (polymorphic, to_edt, to_event) and argv[2] the pair
 * as "<source>-<destination>", each of null, db, event or edt.  Every word
 * but null is a live object the test made: a data block it created and
 * released, an event, an EDT that waits for a dependence nothing satisfies.
 * A call that is let through returns, and the run then ends with status 0 and
 * without the error line.
 */
#include "arts.h"

#include <stdint.h>
#include <string.h>

static void never_runs(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: add_dependence_refused ran an EDT whose dependence was "
              "never satisfied\n");
}

static bool is_word(const char *word, size_t length, const char *name) {
  return strlen(name) == length && strncmp(word, name, length) == 0;
}

/* The GUID a word of the pair stands for; an unknown word ends the run. */
static arts_guid_t guid_of(const char *word, size_t length) {
  if (is_word(word, length, "null")) {
    return NULL_GUID;
  }
  if (is_word(word, length, "db")) {
    void *payload = NULL;
    arts_guid_t block = arts_db_create(&payload, sizeof(uint64_t), ARTS_DB,
                                       ARTS_DB_PROP_NONE, NULL);
    arts_db_release(block);
    return block;
  }
  if (is_word(word, length, "event")) {
    return arts_event_create(NULL);
  }
  if (is_word(word, length, "edt")) {
    return arts_edt_create(never_runs, 0, NULL, 1, NULL);
  }
  arts_printf("FAIL: add_dependence_refused: '%.*s' is none of null, db, "
              "event, edt\n",
              (int)length, word);
  arts_abort(2);
  return NULL_GUID;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  const char *entry = argc > 1 ? argv[1] : "";
  const char *pair = argc > 2 ? argv[2] : "";
  const char *dash = strchr(pair, '-');
  if (dash == NULL) {
    arts_printf("FAIL: add_dependence_refused wants <entry> "
                "<source>-<destination>, got '%s' '%s'\n",
                entry, pair);
    arts_abort(2);
  }
  arts_guid_t source = guid_of(pair, (size_t)(dash - pair));
  arts_guid_t destination = guid_of(dash + 1, strlen(dash + 1));
  if (strcmp(entry, "polymorphic") == 0) {
    arts_add_dependence(source, destination, 3, DB_MODE_NULL);
  } else if (strcmp(entry, "to_edt") == 0) {
    arts_event_add_dependence_to_edt(source, destination, 3, DB_MODE_NULL);
  } else if (strcmp(entry, "to_event") == 0) {
    arts_event_add_dependence_to_event(source, destination, 3);
  } else {
    arts_printf("FAIL: add_dependence_refused: '%s' is none of polymorphic, "
                "to_edt, to_event\n",
                entry);
    arts_abort(2);
  }
  arts_printf("FAIL: add_dependence_refused: the %s call for %s returned\n",
              entry, pair);
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
