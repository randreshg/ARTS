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

#include <stdio.h>
#include <stdlib.h>

#include "arts/arts.h"

static const unsigned int kDefaultBlocks = 16;

static unsigned int numBlocks = 0;
static artsGuid_t *dbGuids = NULL;
static artsGuid_t summaryGuid = NULL_GUID;

static unsigned int parseNumBlocks(int argc, char **argv) {
  if (argc < 2)
    return kDefaultBlocks;

  long value = strtol(argv[1], NULL, 10);
  if (value <= 0)
    return kDefaultBlocks;
  return (unsigned int)value;
}

static void summaryEdt(uint32_t paramc, uint64_t *paramv, uint32_t depc,
                       artsEdtDep_t depv[]) {
  unsigned int totalCreated = 0;
  for (uint32_t i = 0; i < depc; ++i)
    totalCreated += (unsigned int)depv[i].guid;

  printf("[distributed-db-ownership] blocks=%u nodes=%u total_created=%u\n",
         numBlocks, artsGetTotalNodes(), totalCreated);
  artsShutdown();
}

void initPerNode(unsigned int nodeId, int argc, char **argv) {
  numBlocks = parseNumBlocks(argc, argv);
  dbGuids = (artsGuid_t *)artsMalloc(sizeof(artsGuid_t) * numBlocks);
  summaryGuid = artsReserveGuidRoute(ARTS_EDT, 0);

  unsigned int totalNodes = artsGetTotalNodes();
  unsigned int currentNode = artsGetCurrentNode();
  unsigned int locallyOwned = 0;

  for (unsigned int block = 0; block < numBlocks; ++block) {
    unsigned int route = block % totalNodes;
    dbGuids[block] = artsReserveGuidRoute(ARTS_DB_READ, route);
    if (route == currentNode)
      ++locallyOwned;
  }

  printf("[distributed-db-ownership] node=%u reserved=%u owns=%u\n", nodeId,
         numBlocks, locallyOwned);

  if (nodeId == 0) {
    artsEdtCreateWithGuid(summaryEdt, summaryGuid, 0, NULL,
                          artsGetTotalNodes());
  }
}

void initPerWorker(unsigned int nodeId, unsigned int workerId, int argc,
                   char **argv) {
  (void)argc;
  (void)argv;

  if (workerId != 0)
    return;

  unsigned int createdLocal = 0;
  for (unsigned int block = 0; block < numBlocks; ++block) {
    if (!artsIsGuidLocal(dbGuids[block]))
      continue;

    unsigned int *ptr =
        (unsigned int *)artsDbCreateWithGuid(dbGuids[block], sizeof(unsigned int));
    if (!ptr)
      continue;

    *ptr = nodeId;
    ++createdLocal;
  }

  printf("[distributed-db-ownership] node=%u created_local=%u\n", nodeId,
         createdLocal);
  artsSignalEdtValue(summaryGuid, nodeId, createdLocal);
}

int main(int argc, char **argv) {
  artsRT(argc, argv);
  return 0;
}
