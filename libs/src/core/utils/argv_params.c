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
#include "arts.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Words one argument of `len` bytes takes after its length word: the bytes
 * and their NUL, padded to a word. */
static inline uint64_t argv_arg_words(uint64_t len) { return len / 8 + 1; }

uint32_t arts_argv_paramc(int argc, const char *const *argv) {
  if (argc < 0) {
    return 0;
  }
  uint64_t words = 1;
  for (int i = 0; i < argc; i++) {
    words += 1 + argv_arg_words(strlen(argv[i]));
    if (words > UINT32_MAX) {
      return 0;
    }
  }
  return (uint32_t)words;
}

void arts_argv_to_params(int argc, const char *const *argv, uint64_t *paramv) {
  if (argc < 0) {
    return;
  }
  paramv[0] = (uint64_t)argc;
  uint64_t pos = 1;
  for (int i = 0; i < argc; i++) {
    uint64_t len = strlen(argv[i]);
    uint64_t words = argv_arg_words(len);
    paramv[pos] = len;
    memset(&paramv[pos + 1], 0, words * sizeof(uint64_t));
    memcpy(&paramv[pos + 1], argv[i], len);
    pos += 1 + words;
  }
}

/* argc of the block, or -1.  Every word is read only after the count of words
 * left proves it present, and every quantity is 64-bit, so no length word can
 * make the walk wrap or leave [0, paramc). */
static int64_t argv_check(const uint64_t *paramv, uint32_t paramc) {
  if (paramc < 1) {
    return -1;
  }
  uint64_t argc = paramv[0];
  /* Each argument takes at least two words, and argc + 1, the capacity the
   * vector with its NULL needs, fits an int. */
  if (argc >= INT_MAX || argc > ((uint64_t)paramc - 1) / 2) {
    return -1;
  }
  uint64_t pos = 1;
  for (uint64_t i = 0; i < argc; i++) {
    uint64_t words_left = (uint64_t)paramc - pos;
    if (words_left < 2) {
      return -1;
    }
    uint64_t len = paramv[pos];
    /* The bytes and their NUL fit in the words after the length word. */
    if (len >= 8 * (words_left - 1)) {
      return -1;
    }
    if (((const char *)&paramv[pos + 1])[len] != '\0') {
      return -1;
    }
    pos += 1 + argv_arg_words(len);
  }
  return (int64_t)argc;
}

int arts_argv_count(const uint64_t *paramv, uint32_t paramc) {
  return (int)argv_check(paramv, paramc);
}

int arts_argv_from_params(const uint64_t *paramv, uint32_t paramc,
                          const char **argv, int max) {
  int64_t argc = argv_check(paramv, paramc);
  if (argc < 0 || argc >= max) {
    return -1;
  }
  uint64_t pos = 1;
  for (int64_t i = 0; i < argc; i++) {
    uint64_t len = paramv[pos];
    argv[i] = (const char *)&paramv[pos + 1];
    pos += 1 + argv_arg_words(len);
  }
  argv[argc] = NULL;
  return (int)argc;
}
