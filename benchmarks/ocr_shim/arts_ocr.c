/*
 * OCR-to-ARTS Compatibility Shim
 *
 * Implements the OCR v1.2.0 API using ARTS primitives. OCR applications
 * call ocrEdtCreate(), ocrDbCreate(), etc., and this shim translates those
 * calls into the corresponding arts_edt_create(), arts_db_create(), etc.
 *
 * This file is part of the ARTS benchmark infrastructure — it is NOT
 * general-purpose user code. It deliberately accesses ARTS internal headers
 * (route table, runtime types) to implement features like artsDbDataFromGuid().
 *
 * Labeled GUIDs: one rule for every labeled create (data block, event or EDT),
 * whatever properties it carries.  A create whose label names a live object
 * parks where the label's object lives (for a data block, the creating
 * rank's cache or the home) until that object's life ends (a destroy, or an
 * EDT's completion), then installs as the label's next generation; nothing is
 * replaced and no creator is told anything.  GUID_PROP_CHECK and
 * GUID_PROP_BLOCK are accepted and ignored.  Only the create waits: every
 * other message for the next generation (a satisfy, a dependence, including
 * the create-time dependences ocrEdtCreate issues for a labeled EDT) runs
 * against whatever object the label names when it lands, so it must be issued
 * from a point ordered after the previous generation's end through the
 * label's home, or from the home itself.  Two creates of one label with no
 * end of life between them leave one parked for good.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Both ARTS and OCR define enum values with identical names but different
 * semantics (DB_MODE_NULL, DB_MODE_RO, DB_MODE_RW, DB_MODE_RW).
 * ARTS: sequential (0,1,2,3...).  OCR: bitmask (0x0,0x1,0x2,0x4,0x8).
 *
 * Strategy: include OCR headers first (get the real OCR names), then redirect
 * ARTS's enum names through the preprocessor before including ARTS headers.
 * OCR's `typedef u8 bool` also conflicts with C11 <stdbool.h>, so we include
 * OCR before any ARTS/system header that pulls in stdbool.
 */

/* --- OCR headers first --- */
/* OCR extension defines are set via CMake target_compile_definitions. */
#include "extensions/ocr-affinity.h"
#include "ocr-db.h"
#include "ocr-edt.h"
#include "ocr-std.h"
#include "ocr-types.h"
#include "ocr.h"

/* --- Redirect ARTS enum names so they don't collide with OCR's --- */
#define DB_MODE_NULL ARTS_DB_MODE_NULL_
#define DB_MODE_RO ARTS_DB_MODE_RO_
#define DB_MODE_RW ARTS_DB_MODE_RW_

/* OCR defined NULL_GUID as ocrGuid_t struct; save and undef for ARTS */
#undef NULL_GUID

/* ARTS headers pull in <stdbool.h>; OCR already typedef'd bool as u8.
 * Let ARTS redefine bool back to _Bool via stdbool. */

/* --- ARTS headers second --- */
#include "arts.h"
#include "arts/gas/guid.h"
#include "arts/system/identity.h"
#include "arts/utils/malloc.h"
#include "arts/utils/stdatomic_compat.h"

/* Clean up the redirects */
#undef DB_MODE_NULL
#undef DB_MODE_RO
#undef DB_MODE_RW

/* Keep native mode names distinct from the OCR enum. */
#define ARTS_MODE_NULL ((arts_db_access_mode_t)ARTS_DB_MODE_NULL_)
#define ARTS_MODE_RO ((arts_db_access_mode_t)ARTS_DB_MODE_RO_)
#define ARTS_MODE_RW ((arts_db_access_mode_t)ARTS_DB_MODE_RW_)

/* NULL GUID helpers */
#define ARTS_NULL_GUID ((arts_guid_t)0x0)
#define OCR_NULL_GUID ((ocrGuid_t)NULL_GUID_INITIALIZER)

/* =========================================================================
 * Helper: OCR-safe paramv copy.
 *
 * OCR apps commonly cast a smaller struct to (u64*) and pass
 * paramc = ceil(sizeof(struct)/sizeof(u64)).  The last word may
 * extend past the struct allocation by up to 7 bytes.  This is
 * harmless (the source is always on the stack), but ASAN flags the
 * overread.  Copy byte-by-byte for the last partial word to keep
 * ASAN clean while matching original OCR runtime behavior.
 * ========================================================================= */
static inline void ocr_copy_paramv(uint64_t *dst, const u64 *src, u32 paramc) {
  if (paramc > 0 && src != NULL) {
    memcpy(dst, src, paramc * sizeof(uint64_t));
  }
}

#if defined(__SANITIZE_ADDRESS__)
#define _ARTS_OCR_ASAN 1
#elif defined(__clang__)
#if __has_feature(address_sanitizer)
#define _ARTS_OCR_ASAN 1
#endif
#endif
#ifdef _ARTS_OCR_ASAN
/* Under ASAN, suppress the harmless stack overread from struct-to-u64* casts.
 * This matches the original OCR runtime's behavior exactly.
 * We use volatile byte-by-byte copy to avoid the ASAN-intercepted memcpy
 * which checks source bounds even inside no_sanitize functions. */
__attribute__((no_sanitize("address"), noinline)) static void
ocr_copy_paramv_safe(uint64_t *dst, const u64 *src, u32 paramc) {
  volatile const char *s = (volatile const char *)src;
  volatile char *d = (volatile char *)dst;
  for (u32 i = 0; i < paramc * sizeof(uint64_t); i++) {
    d[i] = s[i];
  }
}
#else
#define ocr_copy_paramv_safe ocr_copy_paramv
#endif

/* =========================================================================
 * EDT Template Management
 * ========================================================================= */

typedef struct {
  ocrEdt_t funcPtr;
  u32 paramc;
  u32 depc;
} OcrEdtTemplate;

/* Cross-rank-portable template GUID encoding.
 *
 * A template GUID crosses ranks (an EDT_MOVE carries it), so it cannot be a
 * heap pointer: one rank's heap address is meaningless on another.
 *
 * Benchmarks build non-PIE, so every funcPtr is a fixed absolute
 * address — identical across forked ranks.  Pack the entire template
 * into the GUID itself:
 *
 *   bits 63-44 (20) = depc   (0xFFFFF = EDT_PARAM_UNK; max non-UNK 1048574)
 *   bits 43-28 (16) = paramc (0xFFFF = EDT_PARAM_UNK; max non-UNK 65534)
 *   bits 27-0  (28) = funcPtr (256 MiB; non-PIE x86-64 .text sits in the
 *                     low megabytes)
 *
 * depc gets the widest field: a join EDT's fan-in scales with the problem
 * size (one dependence per tile/box/chunk), while paramc and the text
 * segment do not.  A field that cannot hold the requested count must fail
 * LOUDLY at template creation: OCR programs conventionally ignore the
 * status, so a quiet error here leaves the caller's GUID uninitialized and
 * surfaces later as memory corruption in whatever decodes it.
 *
 * No magic tag is needed: callers always know the GUID came from
 * ocrEdtTemplateCreate when they pass it to ocrEdtCreate or
 * ocrEdtTemplateDestroy, so a runtime distinction from "other"
 * GUIDs is unnecessary. */
#define ARTS_TPL_FUNCPTR_BITS 28
#define ARTS_TPL_FUNCPTR_MASK ((1ULL << ARTS_TPL_FUNCPTR_BITS) - 1)
#define ARTS_TPL_PARAMC_SHIFT ARTS_TPL_FUNCPTR_BITS
#define ARTS_TPL_PARAMC_MASK 0xFFFFu
#define ARTS_TPL_PARAMC_UNK 0xFFFFu
#define ARTS_TPL_PARAMC_MAX 0xFFFEu
#define ARTS_TPL_DEPC_SHIFT (ARTS_TPL_PARAMC_SHIFT + 16)
#define ARTS_TPL_DEPC_MASK 0xFFFFFu
#define ARTS_TPL_DEPC_UNK 0xFFFFFu
#define ARTS_TPL_DEPC_MAX 0xFFFFEu

static inline OcrEdtTemplate arts_tpl_decode(ocrGuid_t g) {
  uint64_t v = (uint64_t)g.guid;
  OcrEdtTemplate t;
  t.funcPtr = (ocrEdt_t)(uintptr_t)(v & ARTS_TPL_FUNCPTR_MASK);
  uint32_t enc_paramc = (uint32_t)((v >> ARTS_TPL_PARAMC_SHIFT) & ARTS_TPL_PARAMC_MASK);
  uint32_t enc_depc = (uint32_t)((v >> ARTS_TPL_DEPC_SHIFT) & ARTS_TPL_DEPC_MASK);
  /* All-ones sentinel = EDT_PARAM_UNK (caller MUST provide explicit value
   * at ocrEdtCreate; passing EDT_PARAM_DEF here is an OCR-app bug). */
  t.paramc = (enc_paramc == ARTS_TPL_PARAMC_UNK) ? EDT_PARAM_UNK : enc_paramc;
  t.depc = (enc_depc == ARTS_TPL_DEPC_UNK) ? EDT_PARAM_UNK : enc_depc;
  return t;
}

u8 ocrEdtTemplateCreate_internal(ocrGuid_t *guid, ocrEdt_t funcPtr, u32 paramc,
                                 u32 depc, const char *funcName) {
  uint64_t fp = (uint64_t)(uintptr_t)funcPtr;
  /* Encoding-capacity violations abort rather than return a status: OCR
   * programs conventionally ignore the return value, so an error here would
   * leave the caller's GUID uninitialized and turn into corruption at the
   * first decode.  There is no valid template to hand back. */
  if ((fp & ~ARTS_TPL_FUNCPTR_MASK) != 0) {
    (void)fprintf(stderr,
                  "ocrEdtTemplateCreate(%s): funcPtr %p exceeds the %d-bit "
                  "template encoding\n",
                  funcName ? funcName : "?", (void *)funcPtr,
                  ARTS_TPL_FUNCPTR_BITS);
    abort();
  }
  /* OCR allows EDT_PARAM_UNK ((u32)-1) for paramc/depc when the count is
   * dynamic at template creation but provided explicitly at every
   * ocrEdtCreate call (reductionLaunch is the canonical user).  Encode
   * EDT_PARAM_UNK as the field's all-ones sentinel; non-UNK values must fit
   * the field. */
  uint32_t enc_paramc;
  if (paramc == EDT_PARAM_UNK) {
    enc_paramc = ARTS_TPL_PARAMC_UNK;
  } else if (paramc > ARTS_TPL_PARAMC_MAX) {
    (void)fprintf(stderr,
                  "ocrEdtTemplateCreate(%s): paramc %" PRIu32
                  " exceeds the template encoding's max %" PRIu32 "\n",
                  funcName ? funcName : "?", paramc,
                  (uint32_t)ARTS_TPL_PARAMC_MAX);
    abort();
  } else {
    enc_paramc = paramc;
  }
  uint32_t enc_depc;
  if (depc == EDT_PARAM_UNK) {
    enc_depc = ARTS_TPL_DEPC_UNK;
  } else if (depc > ARTS_TPL_DEPC_MAX) {
    (void)fprintf(stderr,
                  "ocrEdtTemplateCreate(%s): depc %" PRIu32
                  " exceeds the template encoding's max %" PRIu32 "\n",
                  funcName ? funcName : "?", depc,
                  (uint32_t)ARTS_TPL_DEPC_MAX);
    abort();
  } else {
    enc_depc = depc;
  }
  uint64_t v = fp | ((uint64_t)enc_paramc << ARTS_TPL_PARAMC_SHIFT) |
               ((uint64_t)enc_depc << ARTS_TPL_DEPC_SHIFT);
  guid->guid = (intptr_t)v;
  return 0;
}

u8 ocrEdtTemplateDestroy(ocrGuid_t guid) {
  /* Encoded GUID has no backing allocation. */
  (void)guid;
  return 0;
}

/* Forward declaration: defined alongside ocr_to_arts_mode further down. */
static u32 arts_to_ocr_mode(arts_db_access_mode_t arts_mode);
/* Forward declaration: maps an OCR access mode to the ARTS mode; defined
 * further down.  Used by ocrEdtCreate's create-time depv wiring. */
static arts_db_access_mode_t ocr_to_arts_mode(ocrDbAccessMode_t ocr_mode);

/* Forward declaration: defined alongside the ELS storage further down.
 * Called at the start of every trampoline so ELS truly is "EDT-local"
 * (zero-initialized at the start of each EDT) instead of leaking
 * stale values from the previous EDT that ran on the same worker. */
static void ocr_els_reset(void);

/* =========================================================================
 * OCR EDT Trampoline
 *
 * Layout of ARTS paramv for OCR EDTs:
 *   paramv[0] = function pointer (ocrEdt_t)
 *   paramv[1] = original paramc
 *   paramv[2..2+paramc-1] = original paramv values
 *
 * Non-finish EDTs with an OCR outputEvent are created with
 * hint.output_event; the trampoline registers the OCR return value via
 * arts_edt_set_result, and the ARTS runtime satisfies the output event
 * with it after the EDT's data-block releases (OCR rule: an EDT completes
 * the release of all of its data blocks before its post-event is
 * satisfied).
 *
 * For finish EDTs (EDT_PROP_FINISH), ocrEdtCreate pre-creates a FINISH event
 * (via ARTS_EVENT_HINT_FINISH), passes it to the EDT via hint.finish_event,
 * and chains it to the OCR outputEvent.  The trampoline itself is
 * scope-agnostic.
 * ========================================================================= */

static void ocr_edt_trampoline(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;

  /* Restore EDT-local semantics for the OCR ELS array — see ocr_els_reset. */
  ocr_els_reset();

  ocrEdt_t func = (ocrEdt_t)paramv[0];
  u32 origParamc = (u32)paramv[1];
  /* ARTS paramv is const; copy original params for OCR's non-const API */
  u64 *origParamv = NULL;
  u64 origParamBuf[origParamc > 0 ? origParamc : 1];
  if (origParamc > 0) {
    memcpy(origParamBuf, &paramv[2], origParamc * sizeof(u64));
    origParamv = origParamBuf;
  }

  /* Convert arts_edt_dep_t to ocrEdtDep_t.  Preserve the mode that ARTS
   * resolved during arts_db_acquire_all (RO vs EW vs NULL) — the OCR EDT body
   * may inspect depv[i].mode for assertions or behavior, and surfacing
   * the actual ARTS-resolved mode is more honest than the previous
   * DB_DEFAULT_MODE hardcode. */
  ocrEdtDep_t ocrDepv[depc > 0 ? depc : 1];

  for (u32 i = 0; i < depc; i++) {
    ocrDepv[i].guid.guid = depv[i].guid;
    ocrDepv[i].ptr = depv[i].ptr;
    ocrDepv[i].mode = arts_to_ocr_mode(depv[i].mode);
  }

  ocrGuid_t returnGuid = func(origParamc, origParamv, depc, ocrDepv);
  /* Output-event payload: registered with the runtime, delivered after the
   * EDT's releases.  Unused (and harmless) when the EDT has no output
   * event. */
  arts_edt_set_result(returnGuid.guid);
}

/* =========================================================================
 * Hint Helpers
 *
 * Extract ARTS node route from OCR hint (EDT or DB affinity).
 * OCR apps set affinity via:
 *   ocrSetHintValue(&hint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff))
 * where ocrAffinityToHintValue() returns the node rank as u64.
 * ========================================================================= */

/* One-shot warning when an affinity hint exceeds the available rank
 * count.  Modulo wrap is preserved for compatibility, but the silent
 * wrap can mask app affinity bugs (a typo'd hint silently maps to a
 * different rank).  Warn once globally so the developer notices. */
static void warn_oversized_affinity_once(const char *what, u64 val) {
  /* The CAS winner warns: only its own value. */
  static _Atomic(u32) warned;
  u32 expected = 0;
  if (atomic_compare_exchange_strong_explicit(
          &warned, &expected, 1, memory_order_acq_rel, memory_order_relaxed)) {
    (void)fprintf(
        stderr,
        "[ocr_shim] %s affinity hint %lu exceeds rank count %u, "
        "wrapping via modulo.  Subsequent oversized hints suppressed.\n",
        what, (unsigned long)val, arts_global_rank_count);
  }
}

/* Route extraction for OCR EDT/DB creation.
 *
 * The shim is a thin wrapper: distribution decisions belong to the
 * runtime, not here.  These helpers only translate an explicit
 * OCR_HINT_*_AFFINITY value into a rank.  When no affinity hint is set the
 * caller leaves the choice to the runtime's own no-hint placement of EDTs
 * and data-block homes.
 *
 * Both helpers return -1 when no affinity hint is set, signaling to the
 * caller "no override; apply the no-hint policy". */
static int extract_edt_affinity(ocrHint_t *hint) {
  if (hint == NULL || hint->type != OCR_HINT_EDT_T) {
    return -1;
  }
  int idx = OCR_HINT_EDT_AFFINITY - OCR_HINT_EDT_PROP_START - 1;
  if (idx < 0 || !(hint->propMask & (1ULL << idx))) {
    return -1;
  }
  u64 val = hint->args.propEDT[idx];
  if (val >= arts_global_rank_count) {
    warn_oversized_affinity_once("EDT", val);
  }
  return (int)(val % arts_global_rank_count);
}

static int extract_db_affinity(ocrHint_t *hint) {
  if (hint == NULL || hint->type != OCR_HINT_DB_T) {
    return -1;
  }
  /* Only the affinity hint is consulted.  The other DB hints — the
   * data-movement ones (OCR_HINT_DB_EAGER eager-push, OCR_HINT_DB_LAZY
   * lazy-migrate) and the memory-level ones (NEAR/INTER/FAR/HIGHBW) — are
   * advisory under the OCR spec and intentionally ignored: ARTS coherence is a
   * compile-time global protocol (ARTS_COHERENCE_PROTOCOL), not a per-DB
   * policy, so a per-DB data-movement hint carries no runtime meaning here. */
  int idx = OCR_HINT_DB_AFFINITY - OCR_HINT_DB_PROP_START - 1;
  if (idx < 0 || !(hint->propMask & (1ULL << idx))) {
    return -1;
  }
  u64 val = hint->args.propDB[idx];
  if (val >= arts_global_rank_count) {
    warn_oversized_affinity_once("DB", val);
  }
  return (int)(val % arts_global_rank_count);
}

/* =========================================================================
 * EDT Creation and Management
 * ========================================================================= */

u8 ocrEdtCreate(ocrGuid_t *guid, ocrGuid_t templateGuid, u32 paramc,
                u64 *paramv, u32 depc, ocrGuid_t *depv, u16 properties,
                ocrHint_t *hint, ocrGuid_t *outputEvent) {
  /* Decode the cross-rank-portable template GUID (see arts_tpl_*
   * helpers above).  templateGuid carries the full (funcPtr, paramc,
   * depc) tuple and is identical on every rank thanks to non-PIE
   * binary loading.  Reject NULL_GUID early so funcPtr=NULL never
   * reaches the EDT trampoline. */
  if (templateGuid.guid == 0) {
    return OCR_EINVAL;
  }
  if ((properties & GUID_PROP_IS_LABELED) &&
      (guid == NULL || arts_guid_get_kind(guid->guid) != ARTS_GUID_EDT)) {
    return OCR_EINVAL;
  }
  OcrEdtTemplate templ_local = arts_tpl_decode(templateGuid);
  OcrEdtTemplate *templ = &templ_local;

  u32 actualParamc = (paramc == EDT_PARAM_DEF) ? templ->paramc : paramc;
  u32 actualDepc = (depc == EDT_PARAM_DEF) ? templ->depc : depc;

  /* hint affinity → ARTS rank, else self-rank.  This `rank` places the
   * output/finish event created below (kept local to the creator regardless
   * of where the EDT itself executes); the EDT's own execution rank is
   * `edtRank`, computed separately near its arts_edt_create call below. */
  int aff = extract_edt_affinity(hint);
  unsigned int rank = (aff < 0) ? arts_global_rank_id : (unsigned int)aff;
  arts_guid_t outEvt = NULL_GUID;
  bool isFinishEdt = (properties & EDT_PROP_FINISH) != 0;
  bool oevtValid = (properties & EDT_PROP_OEVT_VALID) != 0;

  if (outputEvent != NULL) {
    if (oevtValid) {
      outEvt = outputEvent->guid;
    } else {
      arts_event_hint_t h = ARTS_EVENT_HINT_DEFAULTS;
      h.rank = rank;
      outEvt = arts_event_create(&h);
      if (outEvt == NULL_GUID) {
        return OCR_ENOMEM;
      }
      outputEvent->guid = outEvt;
    }
  }

  /* paramv layout passed to the trampoline:
   *   [0] = function pointer, [1] = original paramc, [2..2+N-1] = user paramv.
   * arts_calloc zero-initialises, so trailing padding beyond the last user
   * word is safe when OCR apps cast structs to u64* with overshoot. */
  u32 artsParamc = 2 + actualParamc;
  uint64_t *artsParamv = (uint64_t *)arts_calloc(artsParamc, sizeof(uint64_t));
  artsParamv[0] = (uint64_t)(uintptr_t)templ->funcPtr;
  artsParamv[1] = (uint64_t)actualParamc;
  ocr_copy_paramv_safe(&artsParamv[2], paramv, actualParamc);

  /* A finish EDT's scope is a finish event created here, on the creating
   * rank, and handed to the finish EDT as its scope; its descendants inherit
   * it.  Once the finish EDT has joined and the event is chained to outEvt,
   * the shim hands the creator-token back: the finish EDT's own join holds the
   * scope open, so the token would only delay the fire until the creating task
   * returns, which OCR does not require.  The event then fires when the finish
   * EDT and every descendant have completed, satisfying outEvt.  The
   * dependence carries no data (ARTS_MODE_NULL). */
  arts_guid_t fe = NULL_GUID;
  if (isFinishEdt) {
    arts_event_hint_t feh = ARTS_EVENT_HINT_FINISH;
    feh.rank = rank;
    fe = arts_event_create(&feh);
  }

  /* No affinity hint: ARTS_HINT_ANY_RANK lets arts_edt_create's own no-hint
   * placement policy decide.  The hint struct must stay populated either
   * way — it also carries finish_event/output_event below — so unlike
   * arts_db_create's no-hint path this cannot simply pass a NULL hint;
   * ARTS_HINT_ANY_RANK is the struct-preserving equivalent. */
  unsigned int edtRank = (aff < 0) ? ARTS_HINT_ANY_RANK : rank;

  /* Non-finish EDTs deliver the OCR return value through the ARTS output
   * event (hint.output_event; the trampoline registers the value via
   * arts_edt_set_result): the runtime satisfies it after the EDT's
   * data-block releases.  Finish EDTs get their outputEvent chained to the
   * finish event below instead. */
  arts_edt_hint_t edtHint = {.rank = edtRank};
  /* A labeled EDT is created at its label, whose rank is then its home: the
   * GUID's rank is authoritative over any affinity, as for every create of a
   * reserved GUID.  The create-time dependences below reach that home as
   * ordinary messages, so they bind this generation only if the label's
   * previous EDT has already ended. */
  if (properties & GUID_PROP_IS_LABELED) {
    edtHint.guid = guid->guid;
  }
  if (isFinishEdt) {
    edtHint.finish_event = fe;
  } else {
    edtHint.output_event = outEvt;
  }
  arts_guid_t edtGuid = arts_edt_create(ocr_edt_trampoline, artsParamc,
                                        artsParamv, actualDepc, &edtHint);

  if (isFinishEdt && edtGuid != NULL_GUID && fe != NULL_GUID) {
    if (outEvt != NULL_GUID) {
      arts_add_dependence(fe, outEvt, 0, ARTS_MODE_NULL);
    }
    arts_event_release_creator_token(fe);
  }

  arts_free(artsParamv);

  if (edtGuid == NULL_GUID) {
    /* ARTS could not create the EDT (route invalid, OOM, etc.).  Surface
     * the failure as OCR_ENOMEM rather than silently returning success
     * with a bogus GUID. */
    return OCR_ENOMEM;
  }

  if (guid != NULL) {
    guid->guid = edtGuid;
  }

  if (depv != NULL && actualDepc > 0) {
    for (u32 i = 0; i < actualDepc; i++) {
      if (ocrGuidIsNull(depv[i])) {
        /* NULL_GUID = pre-satisfied slot (OCR spec §2.4.3).
         * Signal immediately so the EDT doesn't wait forever. */
        arts_edt_satisfy_slot(edtGuid, i, NULL_GUID, ARTS_MODE_NULL);
      } else if (!ocrGuidIsUninitialized(depv[i])) {
        /* Valid GUID — signal now.  UNINITIALIZED_GUID slots are
         * left open for later ocrAddDependence calls. */
        /* arts_add_dependence handles both DB (immediate satisfy) and
         * event (register waiter) sources uniformly.  The access mode for a
         * create-time dependence is the model's default (read-write): the
         * spec resolves each create-time depv element as if by
         * ocrAddDependence(elem, edt, i, DB_DEFAULT_MODE), and DB_DEFAULT_MODE
         * is RW.  Registering RO here would silently downgrade every
         * create-time DB (or DB-bearing event) dependence to read-only, so an
         * EDT that mutates such a slot loses its publish once it executes on
         * a node other than the DB's home (the shared read-only copy is never
         * published).  Callers that want read-only sharing use an explicit
         * ocrAddDependence(..., DB_MODE_RO) instead of the create-time array. */
        arts_add_dependence(depv[i].guid, edtGuid, i,
                            ocr_to_arts_mode(DB_DEFAULT_MODE));
      }
    }
  }

  return 0;
}

u8 ocrEdtDestroy(ocrGuid_t guid) {
  arts_edt_destroy(guid.guid);
  return 0;
}

/* =========================================================================
 * Event Management
 * ========================================================================= */

/*
 * Map an OCR event flavor + property bits onto a hint snapshot for the
 * unified arts_event_create API.  All OCR
 * flavors collapse onto a single ARTS event type; behavior is selected
 * entirely via hint fields (latch / channel / nb_deps).
 *
 * Default hint = LATCH(1) fire-and-linger (latch=1, auto_destroy=false).
 * Because ARTS is asynchronous, auto-destroying a fired event would race
 * ahead of late (reordered) satisfies/binds, so the lingering single-fire
 * flavors (ONCE/IDEM/STICKY) do NOT auto-destroy — they stay addressable
 * until an explicit destroy.  COUNTED is the one flavor that can be
 * reclaimed safely: its declared consumer count (set from params at the
 * create call) tells the runtime when the last bind has arrived, which is
 * exactly the knowledge reordering takes away from every other flavor.
 * Each case overrides only the fields that diverge.
 */
static arts_event_hint_t ocr_event_kind_to_hint(ocrEventTypes_t kind,
                                                u16 properties) {
  (void)properties;
  /* ONCE/IDEM/STICKY map to the unified LATCH(1) fire-and-linger event; their
   * distinct auto-destroy timing and over-satisfy error are subsumed (silent
   * over-satisfy, linger until explicit destroy).  COUNTED starts from the
   * same base and adds its consumer count at the create call.  CHANNEL is
   * preserved. */
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(1);
  switch (kind) {
  case OCR_EVENT_ONCE_T:
  case OCR_EVENT_IDEM_T:
  case OCR_EVENT_STICKY_T:
  case OCR_EVENT_COUNTED_T:
    break; /* LATCH(1) */
  case OCR_EVENT_LATCH_T:
    /* Counter event; init 0 (caller may override via params). */
    h.latch = 0;
    h.discard_data = true;
    break;
  case OCR_EVENT_CHANNEL_T:
    h = ARTS_EVENT_HINT_CHANNEL;
    break;
  default:
    (void)fprintf(stderr, "[ARTS] unsupported OCR event type %d\n",
                  (int)kind);
    abort();
  }
  return h;
}

u8 ocrEventCreate(ocrGuid_t *guid, ocrEventTypes_t eventType, u16 properties) {
  if (guid == NULL) {
    return OCR_EINVAL;
  }
  arts_event_hint_t h = ocr_event_kind_to_hint(eventType, properties);
  if (properties & GUID_PROP_IS_LABELED) {
    h.guid = guid->guid;
    (void)arts_event_create(&h);
    return 0;
  }
  arts_guid_t g = arts_event_create(&h);
  if (g == NULL_GUID) {
    return OCR_ENOMEM;
  }
  guid->guid = g;
  return 0;
}

u8 ocrEventDestroy(ocrGuid_t guid) {
  arts_event_destroy(guid.guid);
  return 0;
}

u8 ocrEventSatisfy(ocrGuid_t eventGuid, ocrGuid_t dataGuid) {
  arts_event_satisfy_slot(eventGuid.guid, dataGuid.guid,
                          ARTS_EVENT_LATCH_DECR_SLOT);
  return 0;
}

u8 ocrEventSatisfySlot(ocrGuid_t eventGuid, ocrGuid_t dataGuid, u32 slot) {
  arts_event_satisfy_slot(eventGuid.guid, dataGuid.guid, slot);
  return 0;
}

u8 ocrEventCreateParams(ocrGuid_t *guid, ocrEventTypes_t eventType,
                        u16 properties, ocrEventParams_t *params) {
  if (guid == NULL) {
    return OCR_EINVAL;
  }

  arts_event_hint_t h = ocr_event_kind_to_hint(eventType, properties);

  if (eventType == OCR_EVENT_LATCH_T && params != NULL) {
    h.latch = (int32_t)params->EVENT_LATCH.counter;
  }
  if (eventType == OCR_EVENT_COUNTED_T) {
    /* The declared consumer count is the whole of what COUNTED adds, and the
     * only thing that lets a single-fire event be reclaimed: with no count the
     * runtime cannot tell "no more consumers" from "one still in flight",
     * because a bind may legally arrive after the satisfy it depends on.  A
     * zero (or missing) count is therefore not a degenerate COUNTED but a
     * contradiction, and it aborts rather than returning a status the caller
     * conventionally ignores — the reference implementation rejects it too. */
    if (params == NULL || params->EVENT_COUNTED.nbDeps == 0) {
      (void)fprintf(stderr,
                    "[ARTS] OCR_EVENT_COUNTED_T requires a nonzero nbDeps: "
                    "the declared consumer count is what reclaims the event\n");
      abort();
    }
    h.nb_deps = (uint32_t)params->EVENT_COUNTED.nbDeps;
  }
  if (eventType == OCR_EVENT_CHANNEL_T && params != NULL) {
    /* OCR 1.2 §B.5.2: nbSat and nbDeps are restricted to 1.  ARTS enforces
     * that constraint at the shim — generalized values would require a
     * non-trivial change to the channel drain loop (currently fires one
     * data-dep pair per generation). */
    if (params->EVENT_CHANNEL.nbSat != 1 || params->EVENT_CHANNEL.nbDeps != 1) {
      (void)fprintf(stderr,
                    "[ARTS] CHANNEL nbSat=%u nbDeps=%u: only nbSat=nbDeps=1 "
                    "supported (OCR 1.2 §B.5.2)\n",
                    params->EVENT_CHANNEL.nbSat, params->EVENT_CHANNEL.nbDeps);
      return OCR_EINVAL;
    }
    /* maxGen is implementation-driven — ARTS scales unbounded via mpsc. */
  }

  if (properties & GUID_PROP_IS_LABELED) {
    h.guid = guid->guid;
    (void)arts_event_create(&h);
    return 0;
  }
  arts_guid_t g = arts_event_create(&h);
  if (g == NULL_GUID) {
    return OCR_ENOMEM;
  }
  guid->guid = g;
  return 0;
}

/* =========================================================================
 * Data Block Management
 * ========================================================================= */

u8 ocrDbCreate(ocrGuid_t *db, void **addr, u64 len, u16 flags, ocrHint_t *hint,
               ocrInDbAllocator_t allocator) {
  (void)allocator;
  if (db == NULL) {
    return OCR_EINVAL;
  }
  int aff = extract_db_affinity(hint);

  if (flags & GUID_PROP_IS_LABELED) {
    arts_guid_t labeledGuid = db->guid;

    /* DB_PROP_NO_ACQUIRE: same translation as the non-labeled branch below --
     * the creator does not acquire; home stays the sole idle owner and no
     * pointer is handed back.  Otherwise the pointer is this create's image
     * of the block, whether the create installs now or waits for the
     * label's previous block to be destroyed. */
    unsigned int arts_flags = (flags & DB_PROP_NO_ACQUIRE)
                                  ? ARTS_DB_PROP_NO_ACQUIRE
                                  : ARTS_DB_PROP_NONE;
    *addr = arts_db_create_with_guid(labeledGuid, len, ARTS_DB,
                                      arts_flags, NULL);
    return 0;
  }

  /* No affinity hint: a NULL hint defers to arts_db_create's own no-hint
   * home policy. */
  arts_db_hint_t artsHint;
  const arts_db_hint_t *hintp = NULL;
  if (aff >= 0) {
    artsHint = (arts_db_hint_t){.rank = (unsigned int)aff};
    hintp = &artsHint;
  }
  /* DB_PROP_NO_ACQUIRE: the creating EDT does not acquire the block (it is
   * created for a later consumer).  The runtime leaves the home as the sole
   * idle owner and returns a NULL pointer, so no release is required.  Any
   * other property bit maps to the default acquire-on-create behavior. */
  unsigned int arts_flags = (flags & DB_PROP_NO_ACQUIRE)
                                ? ARTS_DB_PROP_NO_ACQUIRE
                                : ARTS_DB_PROP_NONE;
  db->guid = arts_db_create(addr, len, ARTS_DB, arts_flags, hintp);
  if (db->guid == NULL_GUID) {
    return OCR_ENOMEM;
  }

  return 0;
}

u8 ocrDbDestroy(ocrGuid_t db) {
  /* OCR ocrDbDestroy: mark the DB for destruction.  The OCR contract places
   * the liveness guarantee on the caller: the block must no longer be in use
   * (no EDT holds it and none will acquire or await a dependence on it after
   * this point); a dependence resolved against a destroyed DB is undefined.
   * If the calling EDT currently holds an acquire on the block, the destroy
   * implicitly releases it first, matching "ocrDbDestroy releases the DB if the
   * EDT has acquired it".
   *
   * The teardown is deferred: the route-table slot is detached and the storage
   * is reclaimed only once the last outstanding reference drops, so a destroy
   * concurrent with an in-flight lookup is a deferred free, never a
   * use-after-free.  For a coherent block the destroy fans out to every rank
   * that holds a replica or has a request queued, so a parked acquirer is woken
   * to observe the destroyed state (a NULL data resolution) rather than
   * hanging. */
  if (!ocrGuidIsNull(db)) {
    arts_db_destroy(db.guid);
  }
  return 0;
}

u8 ocrDbRelease(ocrGuid_t db) {
  if (!ocrGuidIsNull(db)) {
    arts_db_release(db.guid, ARTS_MODE_RW);
  }
  return 0;
}

/* =========================================================================
 * Dependence Management
 * ========================================================================= */

/*
 * Map OCR access mode → ARTS access mode.
 *
 * ARTS datablocks support RO (shared read) and RW (per-node exclusive write).
 * OCR RW and EW both imply exclusive access, so both map to ARTS RW.  OCR RO
 * maps to ARTS RO.
 *
 * ocrDbRelease() calls arts_db_release(), which releases a hold of either mode
 * before the EDT completes.  A hold must be released before the body's
 * hand-off that can lead to the block's destroy: a destroy is ordered after
 * the release of every hold on the block.
 *
 * IMPORTANT: After the macro cleanup in the header section, bare
 * DB_MODE_RO/EW/RW names resolve to OCR enum constants (0x8/0x4/0x2),
 * NOT ARTS values.  Always use ARTS_MODE_RO/ARTS_MODE_RW for ARTS values.
 */
static arts_db_access_mode_t ocr_to_arts_mode(ocrDbAccessMode_t ocr_mode) {
  switch (ocr_mode) {
  case DB_MODE_EW: /* OCR 0x4 → ARTS RW */
    return ARTS_MODE_RW;
  case DB_MODE_RW: /* OCR 0x2 → ARTS RW */
    return ARTS_MODE_RW;
  case DB_MODE_NULL: /* OCR 0x0 → ARTS NULL (control-only dependence) */
    return ARTS_MODE_NULL;
  case DB_MODE_RO: /* OCR 0x8 → ARTS RO */
  default:
    return ARTS_MODE_RO;
  }
}

/*
 * Inverse mapping for what the EDT body sees in depv[i].mode.
 *
 * ARTS resolves the actual access mode during arts_db_acquire_all.  We surface
 * that to the OCR EDT body so user code (and OCR helper libraries that
 * read depv[i].mode for assertions or branching) sees the truth.
 *
 * RO is reported as DB_MODE_RO rather than DB_DEFAULT_MODE (RW) because
 * the OCR-RW-mapped-to-ARTS-RO path doesn't survive the round trip and
 * we have no way to distinguish "originally RW" from "originally RO".
 * Reporting RO is conservative and matches what arts_db_acquire_all actually
 * did.
 */
static u32 arts_to_ocr_mode(arts_db_access_mode_t arts_mode) {
  switch (arts_mode) {
  case ARTS_DB_MODE_RW_:
    return DB_MODE_RW;
  case ARTS_DB_MODE_NULL_:
    return DB_MODE_NULL;
  case ARTS_DB_MODE_RO_:
  default:
    return DB_MODE_RO;
  }
}

u8 ocrAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot,
                    ocrDbAccessMode_t mode) {

  /* NULL source → signal immediately (slot satisfied with no data). */
  if (ocrGuidIsNull(source)) {
    arts_guid_kind_t dstType = arts_guid_get_kind(destination.guid);
    if (dstType == ARTS_GUID_EDT) {
      arts_edt_satisfy_slot(destination.guid, slot, NULL_GUID, ARTS_MODE_NULL);
    } else if (dstType == ARTS_GUID_EVENT) {
      arts_event_satisfy_slot(destination.guid, NULL_GUID,
                              ARTS_EVENT_LATCH_DECR_SLOT);
    }
    return 0;
  }

  arts_guid_kind_t srcType = arts_guid_get_kind(source.guid);
  arts_guid_kind_t dstType = arts_guid_get_kind(destination.guid);

  if (srcType == ARTS_GUID_DB) {
    /* DB → EDT/Event: arts_add_dependence does immediate satisfy for DB
     * sources (DBs are passive objects — no channel event, no waiting).
     * Map OCR access modes to ARTS: RO→RO, EW/RW→RW (ARTS has no separate
     * EW mode; both OCR EW and RW map to per-node-exclusive DB_MODE_RW).
     * GUID-sorted acquisition in arts_db_acquire_all prevents
     * acquisition-order deadlocks. */
    if (dstType == ARTS_GUID_EDT) {
      arts_add_dependence(source.guid, destination.guid, slot,
                          ocr_to_arts_mode(mode));
    } else if (dstType == ARTS_GUID_EVENT) {
      arts_event_satisfy_slot(destination.guid, source.guid,
                              ARTS_EVENT_LATCH_DECR_SLOT);
    }
  } else if (srcType == ARTS_GUID_EVENT) {
    /* ARTS channels (latch=1) do INCR internally in add_dependence_with_mode.
     * Non-channel events use direct dependent registration. Both paths
     * are handled by arts_add_dependence. Cross-node: handled natively.
     *
     * Pass the user's access mode through ocr_to_arts_mode so the EDT slot
     * is registered with the correct ARTS mode (RO/RW). */
    if (dstType == ARTS_GUID_EDT) {
      arts_add_dependence(source.guid, destination.guid, slot,
                          ocr_to_arts_mode(mode));
    } else if (dstType == ARTS_GUID_EVENT) {
      /* Event→event: OCR spec says "satisfy dest when source fires".
       * Always use DECR slot regardless of the incoming slot param. */
      arts_add_dependence(source.guid, destination.guid,
                          ARTS_EVENT_LATCH_DECR_SLOT, ARTS_MODE_NULL);
    }
  }

  return 0;
}

/* =========================================================================
 * Runtime Control
 * ========================================================================= */

void ocrShutdown(void) { arts_shutdown(); }

void ocrAbort(u8 errorCode) { arts_abort(errorCode); }

/* =========================================================================
 * Printf Support
 * ========================================================================= */

u32 PRINTF(const char *fmt, ...) {
  printf(" [%u] ", arts_global_rank_id);
  va_list args;
  va_start(args, fmt);
  int written = vprintf(fmt, args);
  va_end(args);
  (void)fflush(stdout);
  return (u32)(written >= 0 ? written : 0);
}

u32 ocrPrintf(const char *fmt, ...) {
  /* Use a stack buffer + single write() syscall so concurrent threads
   * cannot interleave mid-line and we never touch glibc's internal
   * FILE* locks, which can deadlock when fflush(0) runs concurrently
   * with another thread holding the FILE* lock (observed under heavy
   * multinode I/O in forked children). */
  char buffer[4096];
  int prefix_len =
      snprintf(buffer, sizeof(buffer), " [%u] ", arts_global_rank_id);
  if (prefix_len < 0 || prefix_len >= (int)sizeof(buffer)) {
    return 0;
  }
  va_list args;
  va_start(args, fmt);
  int body_len =
      vsnprintf(buffer + prefix_len, sizeof(buffer) - prefix_len, fmt, args);
  va_end(args);
  if (body_len < 0) {
    return 0;
  }
  int total = prefix_len + body_len;
  if (total > (int)sizeof(buffer)) {
    total = (int)sizeof(buffer);
  }
  (void)write(STDOUT_FILENO, buffer, (size_t)total);
  return (u32)body_len;
}

u32 SNPRINTF(char *buf, u32 size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf, size, fmt, args);
  va_end(args);
  return (u32)(written >= 0 ? written : 0);
}

void _ocrAssert(u8 val, const char *str, const char *file, u32 line) {
  if (!val) {
    (void)fprintf(stderr, "ASSERTION FAILED: %s at %s:%" PRIu32 "\n", str, file,
                  line);
    abort();
  }
}

/* =========================================================================
 * EDT Local Storage (ELS) Extension
 * ========================================================================= */

#include "extensions/ocr-runtime-itf.h"

#define OCR_ELS_SIZE 16

static _Thread_local ocrGuid_t els_storage[OCR_ELS_SIZE] = {{0}};

/* OCR EDT-Local Storage is supposed to be EDT-scoped: each EDT sees a
 * fresh, zeroed array.  We back it with _Thread_local for performance,
 * which is per-worker, so a stale value from a previously-finished EDT
 * on the same worker would otherwise leak into the next EDT.
 * Trampolines call this at entry to restore EDT-local semantics. */
static void ocr_els_reset(void) {
  memset((void *)els_storage, 0, sizeof(els_storage));
}

ocrGuid_t ocrElsUserGet(u8 offset) {
  if (offset >= OCR_ELS_SIZE) {
    ocrGuid_t null_guid = {0};
    return null_guid;
  }
  return els_storage[offset];
}

void ocrElsUserSet(u8 offset, ocrGuid_t data) {
  if (offset < OCR_ELS_SIZE) {
    els_storage[offset] = data;
  }
}

/* =========================================================================
 * GUID Labeling Extension
 * ========================================================================= */

#include "extensions/ocr-labeling.h"

static arts_guid_kind_t kindToArtsType(ocrGuidUserKind kind) {
  switch (kind) {
  case GUID_USER_DB:
    return ARTS_GUID_DB;
  case GUID_USER_EDT:
  case GUID_USER_EDT_TEMPLATE:
    return ARTS_GUID_EDT;
  case GUID_USER_EVENT_ONCE:
  case GUID_USER_EVENT_COUNTED:
  case GUID_USER_EVENT_IDEM:
  case GUID_USER_EVENT_STICKY:
  case GUID_USER_EVENT_LATCH:
    return ARTS_GUID_EVENT;
  default:
    /* Unknown OCR GUID kind.  Return ARTS_GUID_LAST (out-of-range
     * sentinel) so the downstream arts_guid_reserve_range() rejects it
     * via its `type >= ARTS_GUID_LAST` validation rather than silently
     * producing a range with type bits 0 (ARTS_GUID_RESERVED). */
    return ARTS_GUID_LAST;
  }
}

u8 ocrGuidRangeCreate(ocrGuid_t *rangeGuid, u64 numberGuid,
                      ocrGuidUserKind kind) {
  if (!rangeGuid || numberGuid == 0) {
    return 1;
  }
  arts_guid_kind_t artsType = kindToArtsType(kind);

  /* OCR semantics: any EDT calling this with the same input must end up with
   * the same range GUID, and ocrGuidFromIndex(range, idx) must yield the same
   * GUID on every rank.  Use ARTS's distributed-range mode so that (a) homes
   * are spread across ranks (idx % nrank) and (b) ocrGuidFromIndex is
   * deterministic across ranks once the range GUID is broadcast (the typical
   * mainEdt-creates-and-distributes-via-DB pattern). */
  arts_guid_t range = arts_guid_reserve_range(
      artsType, (unsigned int)numberGuid, ARTS_HINT_ROUND_ROBIN);
  rangeGuid->guid = range;
  return 0;
}

/*
 * ocrGuidMapDestroy: OCR releases a previously-reserved GUID range/map.
 * ARTS can reserve a range (arts_guid_reserve_range) but has no call that
 * returns one, so this is a no-op, and a program that repeatedly creates and
 * destroys GUID ranges consumes GUID space it never gets back.
 */
u8 ocrGuidMapDestroy(ocrGuid_t mapGuid) {
  (void)mapGuid;
  return 0;
}

u8 ocrGuidFromIndex(ocrGuid_t *outGuid, ocrGuid_t rangeGuid, u64 idx) {
  if (!outGuid) {
    return 1;
  }
  outGuid->guid = arts_guid_from_index(rangeGuid.guid, (unsigned int)idx);
  if (ocrGuidIsNull(*outGuid)) {
    return 1;
  }
  return 0;
}

/* =========================================================================
 * Argument Handling
 * ========================================================================= */

u64 getArgc(void *dbPtr) {
  u64 *data = (u64 *)dbPtr;
  return data[0];
}

char *getArgv(void *dbPtr, u64 count) {
  u64 *data = (u64 *)dbPtr;
  u64 offset = data[count + 1];
  return (char *)((u8 *)dbPtr + offset);
}

u64 ocrGetArgc(void *dbPtr) { return getArgc(dbPtr); }

u64 ocrNbWorkers(void) { return arts_get_workers_per_rank(); }

char *ocrGetArgv(void *dbPtr, u64 count) { return getArgv(dbPtr, count); }

/* =========================================================================
 * Affinity Extension
 * ========================================================================= */

u8 ocrAffinityCount(ocrAffinityKind kind, u64 *count) {
  if (!count) {
    return 1;
  }
  switch (kind) {
  case AFFINITY_PD:
    *count = (u64)arts_get_total_ranks();
    break;
  case AFFINITY_CURRENT:
    *count = 1;
    break;
  default:
    *count = 1;
    break;
  }
  return 0;
}

u8 ocrAffinityGet(ocrAffinityKind kind, u64 *count, ocrGuid_t *affinities) {
  if (!count || !affinities) {
    return 1;
  }

  u64 requested = *count;
  u64 available = 0;
  ocrAffinityCount(kind, &available);

  u64 toReturn = (requested < available) ? requested : available;

  switch (kind) {
  case AFFINITY_PD:
    for (u64 i = 0; i < toReturn; i++) {
      affinities[i].guid = (intptr_t)i;
    }
    break;
  case AFFINITY_CURRENT:
    affinities[0].guid = (intptr_t)arts_global_rank_id;
    toReturn = 1;
    break;
  default:
    affinities[0].guid = (intptr_t)arts_global_rank_id;
    toReturn = 1;
    break;
  }

  *count = toReturn;
  return 0;
}

u8 ocrAffinityGetAt(ocrAffinityKind kind, u64 idx, ocrGuid_t *affinity) {
  if (!affinity) {
    return 1;
  }

  u64 count = 0;
  ocrAffinityCount(kind, &count);

  if (idx >= count) {
    return OCR_EINVAL;
  }

  switch (kind) {
  case AFFINITY_PD:
    affinity->guid = (intptr_t)idx;
    break;
  case AFFINITY_CURRENT:
    affinity->guid = (intptr_t)arts_global_rank_id;
    break;
  default:
    affinity->guid = (intptr_t)arts_global_rank_id;
    break;
  }
  return 0;
}

u8 ocrAffinityGetCurrent(ocrGuid_t *affinity) {
  if (!affinity) {
    return 1;
  }
  affinity->guid = (intptr_t)arts_global_rank_id;
  return 0;
}

u8 ocrAffinityQuery(ocrGuid_t guid, u64 *count, ocrGuid_t *affinities) {
  if (!count || !affinities) {
    return 1;
  }
  if (*count >= 1) {
    affinities[0].guid = (intptr_t)ARTS_GUID_GET_RANK((uint64_t)guid.guid);
    *count = 1;
  }
  return 0;
}

u64 ocrAffinityToHintValue(ocrGuid_t affinity) { return (u64)affinity.guid; }

/* =========================================================================
 * Hints Extension
 * ========================================================================= */

u8 ocrHintInit(ocrHint_t *hint, ocrHintType_t hintType) {
  if (!hint) {
    return 1;
  }
  hint->type = hintType;
  hint->propMask = 0;
  memset(&hint->args, 0, sizeof(hint->args));
  return 0;
}

static int getHintPropIndex(ocrHintType_t type, ocrHintProp_t prop) {
  switch (type) {
  case OCR_HINT_EDT_T:
    if (prop > OCR_HINT_EDT_PROP_START && prop < OCR_HINT_EDT_PROP_END) {
      return (int)(prop - OCR_HINT_EDT_PROP_START - 1);
    }
    break;
  case OCR_HINT_DB_T:
    if (prop > OCR_HINT_DB_PROP_START && prop < OCR_HINT_DB_PROP_END) {
      return (int)(prop - OCR_HINT_DB_PROP_START - 1);
    }
    break;
  case OCR_HINT_EVT_T:
    // OCR spec: EVT property range is empty (START == END - 1)
    if (prop > OCR_HINT_EVT_PROP_START && // NOLINT
        prop < OCR_HINT_EVT_PROP_END) {
      return (int)(prop - OCR_HINT_EVT_PROP_START - 1);
    }
    break;
  case OCR_HINT_GROUP_T:
    // OCR spec: GROUP property range is empty (START == END - 1)
    if (prop > OCR_HINT_GROUP_PROP_START && // NOLINT
        prop < OCR_HINT_GROUP_PROP_END) {
      return (int)(prop - OCR_HINT_GROUP_PROP_START - 1);
    }
    break;
  default:
    break;
  }
  return -1;
}

u8 ocrSetHintValue(ocrHint_t *hint, ocrHintProp_t hintProp, u64 value) {
  if (!hint) {
    return 1;
  }

  int idx = getHintPropIndex(hint->type, hintProp);
  if (idx < 0) {
    return OCR_EINVAL;
  }

  switch (hint->type) {
  case OCR_HINT_EDT_T:
    hint->args.propEDT[idx] = value;
    break;
  case OCR_HINT_DB_T:
    hint->args.propDB[idx] = value;
    break;
  case OCR_HINT_EVT_T:
    hint->args.propEVT[idx] = value;
    break;
  case OCR_HINT_GROUP_T:
    hint->args.propGROUP[idx] = value;
    break;
  default:
    return OCR_EINVAL;
  }

  hint->propMask |= (1ULL << idx);
  return 0;
}

u8 ocrUnsetHintValue(ocrHint_t *hint, ocrHintProp_t hintProp) {
  if (!hint) {
    return 1;
  }

  int idx = getHintPropIndex(hint->type, hintProp);
  if (idx < 0) {
    return OCR_EINVAL;
  }

  hint->propMask &= ~(1ULL << idx);
  return 0;
}

u8 ocrGetHintValue(ocrHint_t *hint, ocrHintProp_t hintProp, u64 *value) {
  if (!hint || !value) {
    return 1;
  }

  int idx = getHintPropIndex(hint->type, hintProp);
  if (idx < 0) {
    return OCR_EINVAL;
  }

  if (!(hint->propMask & (1ULL << idx))) {
    return OCR_ENOENT;
  }

  switch (hint->type) {
  case OCR_HINT_EDT_T:
    *value = hint->args.propEDT[idx];
    break;
  case OCR_HINT_DB_T:
    *value = hint->args.propDB[idx];
    break;
  case OCR_HINT_EVT_T:
    *value = hint->args.propEVT[idx];
    break;
  case OCR_HINT_GROUP_T:
    *value = hint->args.propGROUP[idx];
    break;
  default:
    return OCR_EINVAL;
  }

  return 0;
}

/*
 * ocrSetHint / ocrGetHint apply hints to existing GUIDs (post-creation).
 * ARTS fixes an object's placement when it is created: the hint passed to
 * arts_edt_create or arts_db_create is final, and no call moves an EDT or a
 * DB afterwards.  So both are no-ops that report success: ocrSetHint changes
 * no placement, and ocrGetHint leaves the caller's hint as it was.
 */
u8 ocrSetHint(ocrGuid_t guid, ocrHint_t *hint) {
  (void)guid;
  (void)hint;
  return 0;
}

u8 ocrGetHint(ocrGuid_t guid, ocrHint_t *hint) {
  (void)guid;
  (void)hint;
  return 0;
}

/* =========================================================================
 * Main EDT Entry Point
 * ========================================================================= */

extern ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]);

static void mainEdtTrampoline(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;

  ocr_els_reset();

  ocrEdtDep_t ocrDepv[depc > 0 ? depc : 1];
  for (u32 i = 0; i < depc; i++) {
    ocrDepv[i].guid.guid = depv[i].guid;
    ocrDepv[i].ptr = depv[i].ptr;
    ocrDepv[i].mode = arts_to_ocr_mode(depv[i].mode);
  }

  mainEdt(0, NULL, depc, ocrDepv);
}

/* ARTS main_edt entry point */
void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;

  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];

  /* Build OCR argument datablock:
   * Format: [argc][offset0][offset1]...[offsetN-1][arg0\0][arg1\0]... */
  size_t headerSize = sizeof(u64) * (1 + argc);
  size_t stringsSize = 0;
  for (int i = 0; i < argc; i++) {
    stringsSize += strlen(argv[i]) + 1;
  }
  size_t totalSize = headerSize + stringsSize;

  void *dbPtr;
  arts_guid_t argsDbGuid = arts_db_create(&dbPtr, totalSize, ARTS_DB,
                                          ARTS_DB_PROP_NONE, NULL);

  u64 *header = (u64 *)dbPtr;
  header[0] = (u64)argc;

  size_t currentOffset = headerSize;
  for (int i = 0; i < argc; i++) {
    header[i + 1] = currentOffset;
    size_t len = strlen(argv[i]) + 1;
    memcpy((u8 *)dbPtr + currentOffset, argv[i], len);
    currentOffset += len;
  }

  arts_edt_hint_t h = {.rank = arts_global_rank_id};
  arts_guid_t mainEdtGuid = arts_edt_create(mainEdtTrampoline, 0, NULL, 1, &h);
  arts_add_dependence(argsDbGuid, mainEdtGuid, 0, ARTS_MODE_RO);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
