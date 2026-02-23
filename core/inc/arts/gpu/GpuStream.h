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
#ifndef ARTS_GPU_GPUSTREAM_H
#define ARTS_GPU_GPUSTREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <cuda_runtime_api.h>

#include "arts/gas/RouteTable.h"
#include "arts/runtime/RT.h"
#include "arts/system/Debug.h"
#include "arts/utils/ArrayList.h"

// Error codes for ARTS GPU operations
typedef enum {
  ARTS_GPU_SUCCESS = 0,
  ARTS_GPU_ERROR_CUDA_FAILED = 1,
  ARTS_GPU_ERROR_OUT_OF_MEMORY = 2,
  ARTS_GPU_ERROR_INVALID_DEVICE = 3,
  ARTS_GPU_ERROR_KERNEL_FAILED = 4,
  ARTS_GPU_ERROR_SYNC_FAILED = 5,
  ARTS_GPU_ERROR_INVALID_ARG = 6
} artsGpuError_t;

// Legacy fatal error macro - crashes on any error (for backward compatibility)
#define CHECKCORRECT(x)                                                        \
  {                                                                            \
    cudaError_t err;                                                           \
    if ((err = (x)) != cudaSuccess) {                                          \
      PRINTF("FAILED %s: %s\n", #x, cudaGetErrorString(err));                  \
      artsDebugGenerateSegFault();                                             \
    }                                                                          \
  }

// Non-fatal error macro - logs error and returns specified error code
// Use this for operations that can gracefully degrade
#define CUDA_CHECK_RETURN(x, retval)                                           \
  do {                                                                         \
    cudaError_t err = (x);                                                     \
    if (err != cudaSuccess) {                                                  \
      PRINTF("CUDA error in %s at %s:%d: %s (%d)\n", #x, __FILE__, __LINE__,   \
             cudaGetErrorString(err), (int)err);                               \
      return (retval);                                                         \
    }                                                                          \
  } while (0)

// Non-fatal error macro for void functions - logs error and returns
#define CUDA_CHECK_RETURN_VOID(x)                                              \
  do {                                                                         \
    cudaError_t err = (x);                                                     \
    if (err != cudaSuccess) {                                                  \
      PRINTF("CUDA error in %s at %s:%d: %s (%d)\n", #x, __FILE__, __LINE__,   \
             cudaGetErrorString(err), (int)err);                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

// Error check macro that sets an error variable instead of returning
// Useful for cleanup code that needs to continue after errors
#define CUDA_CHECK_SET_ERROR(x, errVar)                                        \
  do {                                                                         \
    cudaError_t err = (x);                                                     \
    if (err != cudaSuccess) {                                                  \
      PRINTF("CUDA error in %s at %s:%d: %s (%d)\n", #x, __FILE__, __LINE__,   \
             cudaGetErrorString(err), (int)err);                               \
      (errVar) = ARTS_GPU_ERROR_CUDA_FAILED;                                   \
    }                                                                          \
  } while (0)

typedef struct {
  unsigned int gpuId;
  volatile unsigned int *newEdtLock;
  artsArrayList *newEdts;
  void *devClosure;
  struct artsEdt *edt;
} artsGpuCleanUp_t;

// Stream type indices for multi-stream support
typedef enum {
  ARTS_GPU_STREAM_H2D = 0,      // Host-to-Device transfers
  ARTS_GPU_STREAM_COMPUTE = 1,  // Kernel execution
  ARTS_GPU_STREAM_D2H = 2,      // Device-to-Host transfers
  ARTS_GPU_STREAM_COUNT = 3     // Total number of streams per GPU
} artsGpuStreamType_t;

typedef struct {
  int device;
  volatile uint64_t availGlobalMem;
  volatile uint64_t totalGlobalMem;
  struct cudaDeviceProp prop;
  volatile float occupancy;
  volatile unsigned int deviceLock;
  volatile unsigned int totalEdts;
  volatile unsigned int availableEdtSlots;
  volatile unsigned int runningEdts;
  volatile unsigned int availableThreads;
  // Multi-stream support: separate streams for H2D, compute, and D2H
  // This enables true overlap of data transfers and kernel execution
  cudaStream_t streams[ARTS_GPU_STREAM_COUNT];
  cudaEvent_t h2dDoneEvent;     // Signals when H2D transfers complete
  cudaEvent_t computeDoneEvent; // Signals when compute completes
  // Legacy single stream (points to compute stream for backward compatibility)
  cudaStream_t stream;
} artsGpu_t;

extern artsGpu_t *artsGpus;

// Error handling and recovery functions
const char *artsGpuErrorString(artsGpuError_t error);
artsGpuError_t artsGpuGetLastError(void);
void artsGpuClearError(void);

// Check if GPU is in a recoverable state after an error
// Returns true if GPU can be used, false if it needs reset
bool artsGpuCanRecover(unsigned int gpuId);

// Attempt to reset a GPU after a fatal error
// Returns ARTS_GPU_SUCCESS on success
artsGpuError_t artsGpuReset(unsigned int gpuId);

void artsNodeInitGpus();
artsGpu_t *artsFindGpu(void *data);

void artsInitPerGpuWrapper(int argc, char **argv);
void artsWorkerInitGpus();
void artsCleanupGpus();
void artsScheduleToGpuInternal(artsEdt_t fnPtr, uint32_t paramc,
                               uint64_t *paramv, uint32_t depc,
                               artsEdtDep_t *depv, dim3 grid, dim3 block,
                               void *edtPtr, artsGpu_t *artsGpu);
void artsScheduleToGpu(artsEdt_t fnPtr, uint32_t paramc, uint64_t *paramv,
                       uint32_t depc, artsEdtDep_t *depv, void *edtPtr,
                       artsGpu_t *artsGpu);
void CUDART_CB artsWrapUp(cudaStream_t stream, cudaError_t status, void *data);
void CUDART_CB artsWrapUpHostFunc(void *data);
void artsGpuSynchronize(artsGpu_t *artsGpu);
void artsGpuStreamBusy(artsGpu_t *artsGpu);
artsGpu_t *artsGpuScheduled(unsigned id);

void artsStoreNewEdts(void *edt);
void artsHandleNewEdts();
void freeGpuItem(artsRouteItem_t *item);

extern __thread dim3 *artsLocalGrid;
extern __thread dim3 *artsLocalBlock;
extern __thread cudaStream_t *artsLocalStream;
extern __thread int artsLocalGpuId;

extern artsGpu_t *artsGpus;

extern volatile unsigned int hits;
extern volatile unsigned int misses;
extern volatile uint64_t freeBytes;

#ifdef __cplusplus
}
#endif

#endif /* ARTSGPUSTREAM_H */
