/******************************************************************************
** Test: Epoch completion must not outrun DB write-back / frontier progress.
******************************************************************************/
#include "arts/arts.h"
#include "arts/system/ArtsPrint.h"

#define NUM_ITERS 8

static artsGuid_t dbGuid = NULL_GUID;
static artsGuid_t parentGuid = NULL_GUID;

void childWriter(uint32_t paramc, uint64_t *paramv, uint32_t depc,
                 artsEdtDep_t depv[]) {
  unsigned int *value = (unsigned int *)depv[0].ptr;
  unsigned int iter = (unsigned int)paramv[0];
  PRINTF("[node %u] child iter %u writing (before=%u)", artsGetCurrentNode(),
         iter, *value);
  *value = iter + 1;
}

void parentLoop(uint32_t paramc, uint64_t *paramv, uint32_t depc,
                artsEdtDep_t depv[]) {
  unsigned int *value = (unsigned int *)depv[0].ptr;
  unsigned int remoteNode = artsGetTotalNodes() > 1 ? 1 : 0;

  PRINTF("[node %u] parent start (initial=%u)", artsGetCurrentNode(), *value);

  for (unsigned int iter = 0; iter < NUM_ITERS; ++iter) {
    PRINTF("[node %u] parent iter %u start (value=%u)", artsGetCurrentNode(),
           iter, *value);
    artsGuid_t epochGuid = artsInitializeAndStartEpoch(NULL_GUID, 0);
    artsGuid_t childGuid =
        artsEdtCreateWithEpoch(childWriter, remoteNode, 1, (uint64_t *)&iter, 1,
                               epochGuid);
    artsRecordDep(dbGuid, childGuid, 0, ARTS_DB_WRITE);
    PRINTF("[node %u] parent iter %u waiting on epoch %lu", artsGetCurrentNode(),
           iter, epochGuid);
    if (!artsWaitOnHandle(epochGuid)) {
      PRINTF("ERROR: epoch wait failed at iter %u", iter);
      artsShutdown();
      return;
    }
    PRINTF("[node %u] parent iter %u done (value=%u)", artsGetCurrentNode(),
           iter, *value);
    if (*value != iter + 1) {
      PRINTF("ERROR: expected %u after iter %u, found %u", iter + 1, iter,
             *value);
      artsShutdown();
      return;
    }
  }

  PRINTF("TEST STATUS: SUCCESS");
  PRINTF("Final value: %u", *value);
  artsShutdown();
}

void initPerNode(unsigned int nodeId, int argc, char **argv) {
  if (!nodeId) {
    dbGuid = artsReserveGuidRoute(ARTS_DB_WRITE, 0);
    parentGuid = artsReserveGuidRoute(ARTS_EDT, 0);
  }
}

void initPerWorker(unsigned int nodeId, unsigned int workerId, int argc,
                   char **argv) {
  if (!nodeId && !workerId) {
    if (artsGetTotalNodes() < 2) {
      PRINTF("ERROR: testEpochDbWritebackOrdering requires at least 2 nodes");
      artsShutdown();
      return;
    }

    unsigned int *value =
        (unsigned int *)artsDbCreateWithGuid(dbGuid, sizeof(unsigned int));
    *value = 0;

    artsEdtCreateWithGuid(parentLoop, parentGuid, 0, NULL, 1);
    PRINTF("[node %u] scheduling parent EDT %lu", artsGetCurrentNode(),
           parentGuid);
    // Parent keeps a READ view across iterations. Each child reacquires WRITE
    // on the same DB in a fresh epoch, reproducing the frontier reuse ordering.
    artsRecordDep(dbGuid, parentGuid, 0, ARTS_DB_READ);
  }
}

int main(int argc, char **argv) {
  artsRT(argc, argv);
  return 0;
}
