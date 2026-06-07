/******************************************************************************
** ARTS GASNet-EX transport backend.
**
** Replaces the rsocket data plane (socket.c) with GASNet-EX. Remote messages
** ride GASNet Active Messages (Medium); wireup is connectionless via
** gex_Client_Init (no per-pair socket mesh, no SSH/connect storm). Process
** launch + rank wireup are handled by the GASNet spawner (PMI under Slurm);
** this file is the data plane only.
**
** This backend implements the same low-level transport symbols the rest of ARTS
** expects from socket.c, so dispatcher.c (arts_server_process_packet),
** protocol.c (the outbound queue + the three *_async enqueue functions) and
** handler.c are reused unchanged. Compiled INSTEAD of socket.c when
** ARTS_USE_GASNET=ON.
**
** Send: a control message is one Medium AM; a header+payload message is sent as
** the concatenation [header][payload]; either is chunked across Medium AMs when
** it exceeds the conduit's max-Medium size and reassembled on arrival, yielding
** the same contiguous packet socket.c delivered.
**
** Receive: GASNet AM handlers run in a restricted context (no blocking, no
** initiating non-reply communication). ARTS message handlers send and allocate,
** so the AM handler only copies the packet into an inbound queue; the ARTS
** receiver thread drains that queue via arts_server_try_to_receive() and runs
** arts_server_process_packet() outside AM-handler context.
**
** A GASNet segment is attached at startup (required by the conduit; also backs
** collectives). True zero-copy bulk transfer (one-sided RMA directly into a
** segment-resident destination datablock) is future work and would be driven
** from handler.c once datablocks are allocated in the segment.
******************************************************************************/
#ifdef ARTS_USE_GASNET

#include <gasnetex.h>

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"
#include "arts/transport/socket.h"
#include "arts/utils/malloc.h"

/* ------------------------------------------------------------------ */
/* GASNet client state                                                */
/* ------------------------------------------------------------------ */
static gex_Client_t g_client;
static gex_EP_t g_ep;
static gex_TM_t g_tm;
static gex_Segment_t g_segment;
static int g_gasnet_up = 0;

/* GASNet segment size per rank (conduit requirement; backs collectives). */
#ifndef ARTS_GEX_SEGMENT_BYTES
#define ARTS_GEX_SEGMENT_BYTES (((size_t)64) << 20) /* 64 MiB */
#endif

/* Single client Active Message handler index (client range is 128..255). */
#define ARTS_GEX_HIDX_PKT 128

/* Referenced as extern by protocol.c. */
unsigned int ports;
bool server_end = false;

/* Per-sender monotonic message id, used to group the chunks of one message. */
static uint64_t g_msgid = 0;

/* ------------------------------------------------------------------ */
/* Inbound completed-packet queue (filled by the AM handler, drained by */
/* the ARTS receiver thread outside AM-handler context).               */
/* ------------------------------------------------------------------ */
struct arts_gex_inbound_s {
  char *buf;
  uint64_t len;
  struct arts_gex_inbound_s *next;
};
static struct arts_gex_inbound_s *g_in_head = NULL;
static struct arts_gex_inbound_s *g_in_tail = NULL;
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;

static void arts_gex_inbound_push(char *buf, uint64_t len) {
  struct arts_gex_inbound_s *node =
      (struct arts_gex_inbound_s *)arts_malloc(sizeof(*node));
  node->buf = buf;
  node->len = len;
  node->next = NULL;
  pthread_mutex_lock(&g_in_lock);
  if (g_in_tail) {
    g_in_tail->next = node;
  } else {
    g_in_head = node;
  }
  g_in_tail = node;
  pthread_mutex_unlock(&g_in_lock);
}

static struct arts_gex_inbound_s *arts_gex_inbound_pop(void) {
  pthread_mutex_lock(&g_in_lock);
  struct arts_gex_inbound_s *node = g_in_head;
  if (node) {
    g_in_head = node->next;
    if (!g_in_head) {
      g_in_tail = NULL;
    }
  }
  pthread_mutex_unlock(&g_in_lock);
  return node;
}

/* ------------------------------------------------------------------ */
/* Reassembly table for messages larger than one Medium AM. Keyed by    */
/* (source rank, message id). Chunks may arrive out of order; each is    */
/* written at its byte offset and a running byte count detects           */
/* completion.                                                           */
/* ------------------------------------------------------------------ */
#define ARTS_GEX_REASM_BUCKETS 256
struct arts_gex_reasm_s {
  uint64_t key;
  char *buf;
  uint64_t total;
  uint64_t got;
  struct arts_gex_reasm_s *next;
};
static struct arts_gex_reasm_s *g_reasm[ARTS_GEX_REASM_BUCKETS];
static pthread_mutex_t g_reasm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Feed one chunk; returns the completed contiguous buffer (caller owns it) once
 * all bytes have arrived, else NULL. */
static char *arts_gex_reasm_feed(uint64_t key, uint64_t total, uint64_t offset,
                                 const void *chunk, uint64_t clen) {
  unsigned bucket = (unsigned)(key % ARTS_GEX_REASM_BUCKETS);
  char *completed = NULL;
  pthread_mutex_lock(&g_reasm_lock);
  struct arts_gex_reasm_s *e = g_reasm[bucket];
  while (e && e->key != key) {
    e = e->next;
  }
  if (!e) {
    e = (struct arts_gex_reasm_s *)arts_malloc(sizeof(*e));
    e->key = key;
    e->buf = (char *)arts_malloc(total);
    e->total = total;
    e->got = 0;
    e->next = g_reasm[bucket];
    g_reasm[bucket] = e;
  }
  memcpy(e->buf + offset, chunk, clen);
  e->got += clen;
  if (e->got >= e->total) {
    struct arts_gex_reasm_s **pp = &g_reasm[bucket];
    while (*pp != e) {
      pp = &(*pp)->next;
    }
    *pp = e->next;
    completed = e->buf;
    arts_free(e);
  }
  pthread_mutex_unlock(&g_reasm_lock);
  return completed;
}

/* ------------------------------------------------------------------ */
/* Active Message handler: receive a (chunk of a) packet.              */
/* AM args: a0 = message id, a1 = byte offset, a2 = total byte length. */
/* ------------------------------------------------------------------ */
static void arts_gex_h_pkt(gex_Token_t token, void *buf, size_t nbytes,
                           gex_AM_Arg_t a0, gex_AM_Arg_t a1, gex_AM_Arg_t a2) {
  gex_Token_Info_t info;
  gex_Token_Info(token, &info, GEX_TI_SRCRANK);
  unsigned int src = (unsigned int)info.gex_srcrank;
  uint32_t msgid = (uint32_t)a0;
  uint64_t offset = (uint64_t)(uint32_t)a1;
  uint64_t total = (uint64_t)(uint32_t)a2;

  if (offset == 0 && (uint64_t)nbytes == total) {
    /* whole message in one Medium AM */
    char *p = (char *)arts_malloc(total);
    memcpy(p, buf, total);
    arts_gex_inbound_push(p, total);
    return;
  }
  uint64_t key = ((uint64_t)src << 32) | (uint64_t)msgid;
  char *done = arts_gex_reasm_feed(key, total, offset, buf, (uint64_t)nbytes);
  if (done) {
    arts_gex_inbound_push(done, total);
  }
}

/* ------------------------------------------------------------------ */
/* Wire send primitives (called by protocol.c's drain loop). Return 0   */
/* = fully sent. GASNet AM with GEX_EVENT_NOW is locally complete on     */
/* return, so there are no partial sends to retry.                       */
/* ------------------------------------------------------------------ */

/* Message larger than one Medium AM: assemble contiguously and chunk. */
static uint64_t arts_gex_send_chunked(int rank, const char *hdr, uint64_t hlen,
                                      const char *payload, uint64_t plen,
                                      uint64_t total, size_t maxmed,
                                      uint32_t msgid) {
  char *tmp = (char *)arts_malloc(total);
  memcpy(tmp, hdr, hlen);
  if (plen) {
    memcpy(tmp + hlen, payload, plen);
  }
  for (uint64_t off = 0; off < total; off += maxmed) {
    uint64_t clen = (total - off < maxmed) ? (total - off) : maxmed;
    gex_AM_RequestMedium(g_tm, (gex_Rank_t)rank, ARTS_GEX_HIDX_PKT, tmp + off,
                         (size_t)clen, GEX_EVENT_NOW, 0, (gex_AM_Arg_t)msgid,
                         (gex_AM_Arg_t)(uint32_t)off,
                         (gex_AM_Arg_t)(uint32_t)total);
  }
  arts_free(tmp);
  return 0;
}

static uint64_t arts_gex_send(int rank, const char *hdr, uint64_t hlen,
                              const char *payload, uint64_t plen) {
  uint64_t total = hlen + plen;
  if (total == 0) {
    return 0;
  }
  size_t maxmed =
      gex_AM_MaxRequestMedium(g_tm, (gex_Rank_t)rank, GEX_EVENT_NOW, 0, 3);
  uint32_t msgid = (uint32_t)__sync_fetch_and_add(&g_msgid, 1U);

  if (total > maxmed) {
    return arts_gex_send_chunked(rank, hdr, hlen, payload, plen, total, maxmed,
                                 msgid);
  }
  if (plen == 0) {
    /* control message: send the header buffer directly, no staging copy */
    gex_AM_RequestMedium(g_tm, (gex_Rank_t)rank, ARTS_GEX_HIDX_PKT, (void *)hdr,
                         (size_t)hlen, GEX_EVENT_NOW, 0, (gex_AM_Arg_t)msgid,
                         (gex_AM_Arg_t)0, (gex_AM_Arg_t)(uint32_t)total);
  } else {
    char *tmp = (char *)arts_malloc(total);
    memcpy(tmp, hdr, hlen);
    memcpy(tmp + hlen, payload, plen);
    gex_AM_RequestMedium(g_tm, (gex_Rank_t)rank, ARTS_GEX_HIDX_PKT, tmp,
                         (size_t)total, GEX_EVENT_NOW, 0, (gex_AM_Arg_t)msgid,
                         (gex_AM_Arg_t)0, (gex_AM_Arg_t)(uint32_t)total);
    arts_free(tmp);
  }
  return 0;
}

uint64_t arts_remote_send_request(int rank, unsigned int queue, char *message,
                                  uint64_t length) {
  (void)queue;
  return arts_gex_send(rank, message, length, NULL, 0);
}

uint64_t arts_remote_send_payload_request(int rank, unsigned int queue,
                                          char *message, unsigned int length,
                                          char *payload, uint64_t length2) {
  (void)queue;
  return arts_gex_send(rank, message, length, payload, length2);
}

/* ------------------------------------------------------------------ */
/* Receive: poll AM (runs handlers -> inbound queue), then drain the    */
/* inbound queue outside AM-handler context.                            */
/* ------------------------------------------------------------------ */
bool arts_server_try_to_receive(char **in_buffer, const int *in_packet_size,
                                const volatile unsigned int *remote_steal_lock) {
  (void)in_buffer;
  (void)in_packet_size;
  (void)remote_steal_lock;
  if (!g_gasnet_up) {
    return false;
  }
  gasnet_AMPoll();
  bool did = false;
  struct arts_gex_inbound_s *node;
  while ((node = arts_gex_inbound_pop()) != NULL) {
    arts_server_process_packet((struct arts_remote_packet_s *)node->buf);
    arts_free(node->buf);
    arts_free(node);
    did = true;
  }
  return did;
}

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                    */
/* ------------------------------------------------------------------ */
void arts_ll_server_setup(struct arts_config_s *config) {
  gex_Client_Init(&g_client, &g_ep, &g_tm, "arts", NULL, NULL, 0);

  gex_AM_Entry_t htable[] = {
      {ARTS_GEX_HIDX_PKT, (gex_AM_Fn_t)arts_gex_h_pkt,
       GEX_FLAG_AM_REQUEST | GEX_FLAG_AM_MEDIUM, 3, NULL, "arts_pkt"},
  };
  gex_EP_RegisterHandlers(g_ep, htable, sizeof(htable) / sizeof(htable[0]));
  gex_Segment_Attach(&g_segment, g_tm, (size_t)ARTS_GEX_SEGMENT_BYTES);

  /* Rank/size come from GASNet, not the config file. */
  arts_global_rank_id = (unsigned int)gex_TM_QueryRank(g_tm);
  arts_global_rank_count = (unsigned int)gex_TM_QuerySize(g_tm);
  ports = config->port_count ? config->port_count : 1;

  /* The GASNet spawner already launched every rank, so ARTS must not spawn. */
  config->master_boot = false;
  config->table_length = arts_global_rank_count;
  config->nodes = arts_global_rank_count;

  g_gasnet_up = 1;
  ARTS_INFO("ARTS transport: %s; GASNet up rank %u/%u, max_medium=%lu",
            arts_transport_kind_name(), arts_global_rank_id,
            arts_global_rank_count,
            (unsigned long)gex_AM_MaxRequestMedium(g_tm, GEX_RANK_INVALID,
                                                   GEX_EVENT_NOW, 0, 3));
}

void arts_ll_server_shutdown() { /* shutdown messages handled by dispatcher.c */ }

void arts_ll_server_wakeup_receivers() {
  if (g_gasnet_up) {
    gasnet_AMPoll();
  }
}

void arts_ll_server_cleanup() {
  if (g_gasnet_up) {
    /* Quiesce so no rank tears down the conduit while a peer is still using it. */
    gex_Event_Wait(gex_Coll_BarrierNB(g_tm, 0));
  }
}

/* ------------------------------------------------------------------ */
/* Remaining socket.c surface. GASNet is connectionless and spawner-    */
/* launched, so connection setup and the socket helpers are no-ops.     */
/* ------------------------------------------------------------------ */
unsigned int arts_remote_get_my_rank() { return arts_global_rank_id; }

bool arts_transport_runtime_uses_rdma(void) { return true; }

// Public transport-kind name for the GASNet backend. Distinct from the
// rsocket/TCP socket.c backend so logs and callers can tell GASNet apart from
// the legacy rdma-rsocket data plane.
const char *arts_transport_kind_name(void) { return "gasnet"; }

void arts_remote_set_message_table(struct arts_config_s *table) { (void)table; }

void arts_remote_setup_outgoing() {}

bool arts_remote_setup_incoming() { return true; }

void arts_remote_eager_connect_all() {}

void arts_remote_refresh_startup_connect_grace() {}

void arts_remote_set_thread_inbound_queues(unsigned int start,
                                           unsigned int stop) {
  (void)start;
  (void)stop;
}

void arts_remote_thread_inbound_queues_cleanup() {}

bool max_out_buffs(unsigned int ignore) {
  (void)ignore;
  return true;
}

int arts_get_new_socket() { return -1; }
void arts_server_set_socket_options_sender(unsigned int socket) { (void)socket; }
void arts_server_set_socket_options_reciever(unsigned int socket) {
  (void)socket;
}
int arts_get_socket_listening(struct sockaddr_in *listening_socket,
                              unsigned int port) {
  (void)listening_socket;
  (void)port;
  return -1;
}
int arts_get_socket_outgoing(struct sockaddr_in *outgoing_socket,
                             unsigned int port, in_addr_t s_addr) {
  (void)outgoing_socket;
  (void)port;
  (void)s_addr;
  return -1;
}

#endif /* ARTS_USE_GASNET */
