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
#include "arts/transport/net.h"
#include "arts/transport/socket.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "arts/system/identity.h" /* arts_global_rank_id / arts_global_rank_count */
#include "arts/system/print.h"
#include "arts/system/shutdown.h"
#include "arts/utils/malloc.h"

/* One peer's side of the exchange: the frame read from it so far and the
 * bytes of this rank's frame written to it. */
struct exchange_peer_s {
  struct arts_net_addr_frame_s in;
  size_t got;
  size_t sent;
  bool active;
};

static uint64_t exchange_now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

/* Reads what is left of the peer's frame, never a byte past it; true when
 * the peer's end showed. */
static bool exchange_read(struct exchange_peer_s *e, unsigned int p,
                          unsigned int n, unsigned int own_len) {
  const size_t head = offsetof(struct arts_net_addr_frame_s, addr);
  for (;;) {
    const size_t whole = head + (e->got >= head ? e->in.len : 0u);
    if (e->got >= head && e->got == whole) {
      return false;
    }
    const size_t want = (e->got < head ? head : whole) - e->got;
    const ssize_t r =
        recv(arts_socket_fd[p], (uint8_t *)&e->in + e->got, want, MSG_DONTWAIT);
    if (r > 0) {
      e->got += (size_t)r;
      if (e->got == head) {
        if (e->in.rank != p) {
          ARTS_ERROR("arts_net_exchange: rank %u's connection carries a frame "
                     "naming rank %u",
                     p, e->in.rank);
        }
        if (!arts_net_addr_frame_ok(e->in.rank, e->in.len, n, own_len)) {
          ARTS_ERROR("arts_net_exchange: bad address frame (rank=%u len=%u; "
                     "expected rank<%u len=%u) — a length other than this "
                     "rank's means the peer's endpoint carries another "
                     "address format",
                     e->in.rank, e->in.len, n, own_len);
        }
      }
      continue;
    }
    if (r < 0 && errno == EINTR) {
      continue;
    }
    return !(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  }
}

/* Writes what is left of this rank's frame to the peer; true when the
 * peer's end showed. */
static bool exchange_write(struct exchange_peer_s *e, unsigned int p,
                           const void *out, size_t out_bytes) {
  while (e->sent < out_bytes) {
    const ssize_t r = send(arts_socket_fd[p], (const uint8_t *)out + e->sent,
                           out_bytes - e->sent, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r > 0) {
      e->sent += (size_t)r;
    } else if (r < 0 && errno == EINTR) {
      continue;
    } else {
      return !(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
  }
  return false;
}

uint64_t arts_net_exchange_addresses(uint64_t run_id) {
  const unsigned int n = arts_global_rank_count;
  const unsigned int self = arts_global_rank_id;

  /* Own address blob (fixed size within a homogeneous provider run). */
  uint8_t own[ARTS_NET_ADDR_MAX];
  const unsigned int own_len = arts_net_own_address(own, sizeof(own));
  if (own_len == 0 || own_len > ARTS_NET_ADDR_MAX) {
    ARTS_ERROR("arts_net_exchange: own address length %u out of range", own_len);
  }

  /* Rank-indexed contiguous address table (stride = own_len). */
  uint8_t *table = (uint8_t *)arts_calloc(n, own_len);
  memcpy(table + (size_t)self * own_len, own, own_len);

  struct arts_net_addr_frame_s out;
  out.rank = self;
  out.len = own_len;
  out.run_id = run_id;
  memcpy(out.addr, own, own_len);
  const size_t head = offsetof(struct arts_net_addr_frame_s, addr);
  const size_t frame_bytes = head + own_len;

  struct exchange_peer_s *peer =
      (struct exchange_peer_s *)arts_calloc(n, sizeof(struct exchange_peer_s));
  struct pollfd *pfd = (struct pollfd *)arts_calloc(n, sizeof(struct pollfd));
  unsigned int *who = (unsigned int *)arts_calloc(n, sizeof(unsigned int));
  bool *flagged = (bool *)arts_calloc(n, sizeof(bool));
  for (unsigned int p = 0; p < n; p++) {
    peer[p].active = p != self;
  }
  const uint64_t deadline = arts_startup_deadline_ns();
  uint64_t run = run_id;
  for (;;) {
    if (arts_exit_began()) {
      arts_exit(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR);
    }
    unsigned int k = 0;
    for (unsigned int p = 0; p < n; p++) {
      if (!peer[p].active) {
        continue;
      }
      short events = POLLRDHUP;
      if (peer[p].got < frame_bytes) {
        events |= POLLIN;
      }
      if (peer[p].sent < frame_bytes) {
        events |= POLLOUT;
      }
      pfd[k] = (struct pollfd){.fd = arts_socket_fd[p], .events = events};
      who[k++] = p;
    }
    if (k == 0) {
      break;
    }
    const uint64_t now = exchange_now_ns();
    if (now >= deadline) {
      for (unsigned int p = 0; p < n; p++) {
        flagged[p] = peer[p].active && peer[p].got < frame_bytes;
      }
      arts_mesh_fail(ARTS_MESH_PHASE_EXCHANGE, flagged);
    }
    const uint64_t ms = (deadline - now + 999999u) / 1000000u;
    if (poll(pfd, (nfds_t)k, ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms) < 0) {
      if (errno == EINTR) {
        continue;
      }
      ARTS_ERROR("arts_net_exchange: poll: %s", strerror(errno));
    }
    bool ended_any = false;
    memset(flagged, 0, n * sizeof(bool));
    for (unsigned int i = 0; i < k; i++) {
      const short re = pfd[i].revents;
      if (re == 0) {
        continue;
      }
      const unsigned int p = who[i];
      struct exchange_peer_s *e = &peer[p];
      bool ended = (re & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
      if (e->sent < frame_bytes) {
        ended |= exchange_write(e, p, &out, frame_bytes);
      }
      if (e->got < frame_bytes) {
        ended |= exchange_read(e, p, n, own_len);
        if (e->got == frame_bytes) {
          memcpy(table + (size_t)p * own_len, e->in.addr, own_len);
          if (p == 0) {
            run = e->in.run_id;
          }
        }
      }
      /* A peer cannot arm before it holds this rank's whole frame, and its
       * own frame precedes its end, so an end proves it never armed only
       * while either frame is unfinished; after both, it is left to the
       * sentinels. */
      if (e->got == frame_bytes && e->sent == frame_bytes) {
        e->active = false;
      } else if (ended) {
        flagged[p] = true;
        ended_any = true;
      }
    }
    if (ended_any) {
      arts_mesh_fail(ARTS_MESH_PHASE_HANGUP, flagged);
    }
  }

  /* Insert the whole table in rank order so fi_addr_t == rank. */
  arts_net_av_insert_table(table, own_len, n);
  arts_free(flagged);
  arts_free(who);
  arts_free(pfd);
  arts_free(peer);
  arts_free(table);
  ARTS_INFO("arts_net: address exchange complete (%u ranks, %u-byte addrs)", n,
            own_len);
  return run;
}
