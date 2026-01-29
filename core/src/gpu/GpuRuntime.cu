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

// Some help https://devblogs.nvidia.com/how-overlap-data-transfers-cuda-cc/
// and
// https://github.com/NVIDIA-developer-blog/code-samples/blob/master/series/cuda-cpp/overlap-data-transfers/async.cu
// Once this *class* works we will put a stream(s) in create a thread local
// stream.  Then we will push stuff!
#include "arts/gpu/GpuRuntime.cuh"

#include <cuda.h>  // CUDA Driver API for PTX loading
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "arts/gas/OutOfOrder.h"
#include "arts/gpu/GpuLCSyncFunctions.cuh"
#include "arts/gpu/GpuRouteTable.h"
#include "arts/gpu/GpuStream.h"
#include "arts/gpu/GpuStreamBuffer.h"
#include "arts/introspection/Metrics.h"
#include "arts/runtime/Globals.h"
#include "arts/runtime/Runtime.h"
#include "arts/runtime/compute/EdtFunctions.h"
#include "arts/runtime/memory/DbFunctions.h"
#include "arts/runtime/sync/TerminationDetection.h"
#include "arts/system/ArtsPrint.h"
#include "arts/system/Debug.h"
#include "arts/utils/Atomics.h"
#include "arts/utils/Deque.h"

__thread int artsSavedDeviceId = -1;
__thread int artsCurrentDeviceId = -1;

int artsGetCurrentGpu() {
  if (artsCurrentDeviceId == -1)
    CHECKCORRECT(cudaGetDevice(&artsCurrentDeviceId));

  return artsCurrentDeviceId;
}

bool artsCudaSetDevice(int id, bool save) {
  if (artsCurrentDeviceId == -1)
    CHECKCORRECT(cudaGetDevice(&artsCurrentDeviceId));

  if (save)
    artsSavedDeviceId = artsCurrentDeviceId;

  if (id > -1 && id < artsNodeInfo.gpu && id != artsCurrentDeviceId) {
    CHECKCORRECT(cudaSetDevice(id));
    artsCurrentDeviceId = id;
    return true;
  }

  return false;
}

bool artsCudaRestoreDevice() {
  return artsCudaSetDevice(artsSavedDeviceId, false);
}

void *artsCudaMallocHost(unsigned int size) {
  void *ptr = NULL;
  CHECKCORRECT(cudaMallocHost(&ptr, size));
  // ptr = artsCalloc(1, size);
  if (!ptr) {
    artsDebugPrintStack();
    exit(1);
  }
  return ptr;
}

void artsCudaFreeHost(void *ptr) {
  if (ptr)
    CHECKCORRECT(cudaFreeHost(ptr));
  // artsFree(ptr);
}

// Stream-ordered allocation support (CUDA 11.2+)
// When enabled, allocations are tied to a specific stream for better concurrency
#if CUDART_VERSION >= 11020
#define ARTS_STREAM_ORDERED_ALLOC_SUPPORTED 1
#else
#define ARTS_STREAM_ORDERED_ALLOC_SUPPORTED 0
#endif

void *artsCudaMalloc(unsigned int size) {
  void *ptr = NULL;
  CHECKCORRECT(cudaMalloc(&ptr, size));
  if (!ptr) {
    ARTS_INFO("artsCudaMalloc failed %lu\n",
              artsGpus[artsCurrentDeviceId].availGlobalMem);
    artsDebugPrintStack();
    exit(1);
  }
  return ptr;
}

// Stream-ordered allocation - allocates memory in the stream's memory pool
// Memory is available immediately for use in the same stream
// For CUDA < 11.2, falls back to regular cudaMalloc
void *artsCudaMallocAsync(unsigned int size, cudaStream_t stream) {
  void *ptr = NULL;
#if ARTS_STREAM_ORDERED_ALLOC_SUPPORTED
  cudaError_t err = cudaMallocAsync(&ptr, size, stream);
  if (err != cudaSuccess) {
    // Fallback to synchronous allocation if async fails
    ARTS_DEBUG("cudaMallocAsync failed, falling back to cudaMalloc\n");
    CHECKCORRECT(cudaMalloc(&ptr, size));
  }
#else
  CHECKCORRECT(cudaMalloc(&ptr, size));
#endif
  if (!ptr) {
    ARTS_INFO("artsCudaMallocAsync failed %lu\n",
              artsGpus[artsCurrentDeviceId].availGlobalMem);
    artsDebugPrintStack();
    exit(1);
  }
  return ptr;
}

void artsCudaFree(void *ptr) {
  if (ptr)
    CHECKCORRECT(cudaFree(ptr));
}

// Stream-ordered free - returns memory to the stream's memory pool
// Memory may be reused by subsequent allocations in the same stream
// For CUDA < 11.2, falls back to regular cudaFree
void artsCudaFreeAsync(void *ptr, cudaStream_t stream) {
  if (!ptr) return;
#if ARTS_STREAM_ORDERED_ALLOC_SUPPORTED
  cudaError_t err = cudaFreeAsync(ptr, stream);
  if (err != cudaSuccess) {
    // Fallback to synchronous free if async fails
    ARTS_DEBUG("cudaFreeAsync failed, falling back to cudaFree\n");
    CHECKCORRECT(cudaFree(ptr));
  }
#else
  CHECKCORRECT(cudaFree(ptr));
#endif
}

void artsCudaMemCpyFromDev(void *dst, void *src, size_t count) {
  CHECKCORRECT(cudaMemcpy(dst, src, count, cudaMemcpyDeviceToHost));
}

void artsCudaMemCpyToDev(void *dst, void *src, size_t count) {
  CHECKCORRECT(cudaMemcpy(dst, src, count, cudaMemcpyHostToDevice));
}

dim3 *artsGetGpuGrid() { return artsLocalGrid; }

dim3 *artsGetGpuBlock() { return artsLocalBlock; }

cudaStream_t *artsGetGpuStream() { return artsLocalStream; }

int artsGetGpuId() { return artsLocalGpuId; }

unsigned int artsGetNumGpus() { return artsNodeInfo.gpu; }

artsGuid_t internalEdtCreateGpu(artsEdt_t funcPtr, artsGuid_t *guid,
                                unsigned int route, uint32_t paramc,
                                uint64_t *paramv, uint32_t depc, dim3 grid,
                                dim3 block, artsGuid_t endGuid, uint32_t slot,
                                artsGuid_t dataGuid, bool hasDepv,
                                bool passThrough, bool lib, int gpuToRunOn) {
  ARTS_INFO("Creating GPU EDT: func=%p, grid=(%d,%d,%d), block=(%d,%d,%d), route=%u, depc=%u, lib=%d, gpuToRunOn=%d\n",
            funcPtr, grid.x, grid.y, grid.z, block.x, block.y, block.z, route, depc, lib, gpuToRunOn);
  //    ARTSEDTCOUNTERTIMERSTART(edtCreateCounter);
  unsigned int depSpace = (hasDepv) ? depc * sizeof(artsEdtDep_t) : 0;
  unsigned int edtSpace =
      sizeof(artsGpuEdt_t) + paramc * sizeof(uint64_t) + depSpace;

  artsGpuEdt_t *edt = (artsGpuEdt_t *)artsCallocAlignWithType(
      1, edtSpace, 16, artsEdtMemorySize);
  if (!edt) {
    ARTS_INFO("Creating PTX GPU EDT: allocation failed (size=%u)\n", edtSpace);
    return NULL_GUID;
  }
  edt->wrapperEdt.invalidateCount = 1;
  edt->grid = grid;
  edt->block = block;
  edt->gpuToRunOn = gpuToRunOn;
  edt->endGuid = endGuid;
  edt->slot = slot;
  edt->dataGuid = dataGuid;
  edt->passthrough = passThrough;
  edt->lib = lib;

  // artsIntrospectionEdtCreateBegin();
  bool created = artsEdtCreateInternal(
      (struct artsEdt *)edt, ARTS_GPU_EDT, guid, route,
      artsThreadInfo.clusterId, edtSpace, NULL_GUID, funcPtr, paramc, paramv,
      depc, true, NULL_GUID, hasDepv, 0);
  ARTS_INFO("GPU EDT created: guid=%lu, created=%d, depc=%u, hasDepv=%d\n", *guid, created, depc, hasDepv);
  // artsIntrospectionEdtCreateFinish(created);
  //    ARTSEDTCOUNTERTIMERENDINCREMENT(edtCreateCounter);
  return *guid;
}

artsGuid_t artsEdtCreateGpuDep(artsEdt_t funcPtr, unsigned int route,
                               uint32_t paramc, uint64_t *paramv, uint32_t depc,
                               dim3 grid, dim3 block, artsGuid_t endGuid,
                               uint32_t slot, artsGuid_t dataGuid,
                               bool hasDepv) {
  artsGuid_t guid = NULL_GUID;
  return internalEdtCreateGpu(funcPtr, &guid, route, paramc, paramv, depc, grid,
                              block, endGuid, slot, dataGuid, hasDepv, false,
                              false, -1);
}

artsGuid_t artsEdtCreateGpuPTDep(artsEdt_t funcPtr, unsigned int route,
                                 uint32_t paramc, uint64_t *paramv,
                                 uint32_t depc, dim3 grid, dim3 block,
                                 artsGuid_t endGuid, uint32_t slot,
                                 unsigned int passSlot, bool hasDepv) {
  artsGuid_t guid = NULL_GUID;
  return internalEdtCreateGpu(funcPtr, &guid, route, paramc, paramv, depc, grid,
                              block, endGuid, slot, (artsGuid_t)passSlot,
                              hasDepv, true, false, -1);
}

artsGuid_t artsEdtCreateGpu(artsEdt_t funcPtr, unsigned int route,
                            uint32_t paramc, uint64_t *paramv, uint32_t depc,
                            dim3 grid, dim3 block, artsGuid_t endGuid,
                            uint32_t slot, artsGuid_t dataGuid) {
  return artsEdtCreateGpuDep(funcPtr, route, paramc, paramv, depc, grid, block,
                             endGuid, slot, dataGuid, true);
}

artsGuid_t artsEdtCreateGpuWithGuid(artsEdt_t funcPtr, artsGuid_t guid,
                                    uint32_t paramc, uint64_t *paramv,
                                    uint32_t depc, dim3 grid, dim3 block,
                                    artsGuid_t endGuid, uint32_t slot,
                                    artsGuid_t dataGuid) {
  return internalEdtCreateGpu(funcPtr, &guid, artsGuidGetRank(guid), paramc,
                              paramv, depc, grid, block, endGuid, slot,
                              dataGuid, true, false, false, -1);
}

artsGuid_t artsEdtCreateGpuPT(artsEdt_t funcPtr, unsigned int route,
                              uint32_t paramc, uint64_t *paramv, uint32_t depc,
                              dim3 grid, dim3 block, artsGuid_t endGuid,
                              uint32_t slot, unsigned int passSlot) {
  return artsEdtCreateGpuPTDep(funcPtr, route, paramc, paramv, depc, grid,
                               block, endGuid, slot, passSlot, true);
}

artsGuid_t artsEdtCreateGpuPTWithGuid(artsEdt_t funcPtr, artsGuid_t guid,
                                      uint32_t paramc, uint64_t *paramv,
                                      uint32_t depc, dim3 grid, dim3 block,
                                      artsGuid_t endGuid, uint32_t slot,
                                      unsigned int passSlot) {
  return internalEdtCreateGpu(funcPtr, &guid, artsGuidGetRank(guid), paramc,
                              paramv, depc, grid, block, endGuid, slot,
                              (artsGuid_t)passSlot, true, true, false, -1);
}

artsGuid_t artsEdtCreateGpuLib(artsEdt_t funcPtr, unsigned int route,
                               uint32_t paramc, uint64_t *paramv, uint32_t depc,
                               dim3 grid, dim3 block) {
  artsGuid_t guid = NULL_GUID;
  return internalEdtCreateGpu(funcPtr, &guid, route, paramc, paramv, depc, grid,
                              block, NULL_GUID, 0, NULL_GUID, true, false, true,
                              -1);
}

artsGuid_t artsEdtCreateGpuLibWithGuid(artsEdt_t funcPtr, artsGuid_t guid,
                                       uint32_t paramc, uint64_t *paramv,
                                       uint32_t depc, dim3 grid, dim3 block) {
  return internalEdtCreateGpu(funcPtr, &guid, artsGuidGetRank(guid), paramc,
                              paramv, depc, grid, block, NULL_GUID, 0,
                              NULL_GUID, true, false, true, -1);
}

artsGuid_t artsEdtCreateGpuDirect(artsEdt_t funcPtr, unsigned int route,
                                  unsigned int gpu, uint32_t paramc,
                                  uint64_t *paramv, uint32_t depc, dim3 grid,
                                  dim3 block, artsGuid_t endGuid, uint32_t slot,
                                  artsGuid_t dataGuid, bool hasDepv) {
  artsGuid_t guid = NULL_GUID;
  return internalEdtCreateGpu(funcPtr, &guid, route, paramc, paramv, depc, grid,
                              block, endGuid, slot, dataGuid, hasDepv, false,
                              false, gpu);
}

artsGuid_t artsEdtCreateGpuLibDirect(artsEdt_t funcPtr, unsigned int route,
                                     unsigned int gpu, uint32_t paramc,
                                     uint64_t *paramv, uint32_t depc, dim3 grid,
                                     dim3 block) {
  artsGuid_t guid = NULL_GUID;
  return internalEdtCreateGpu(funcPtr, &guid, route, paramc, paramv, depc, grid,
                              block, NULL_GUID, 0, NULL_GUID, true, false, true,
                              gpu);
}

//=============================================================================
// PTX Module Loading and Caching
//=============================================================================

// Module cache: key = content hash of PTX source, value = array of CUmodule per GPU
// Using content hash instead of pointer prevents redundant compilations when
// the same PTX content is loaded from different memory locations
static std::map<size_t, std::vector<CUmodule>> ptxModuleCache;
static std::mutex ptxCacheMutex;
static bool cuInitialized = false;

// Hash function for PTX content (FNV-1a hash)
static size_t hashPtxContent(const char* ptxSource) {
  if (!ptxSource) return 0;
  size_t hash = 14695981039346656037ULL;  // FNV offset basis
  const char* p = ptxSource;
  while (*p) {
    hash ^= static_cast<size_t>(*p);
    hash *= 1099511628211ULL;  // FNV prime
    ++p;
  }
  return hash;
}

// Initialize CUDA Driver API (idempotent)
static CUresult ensureCuInit() {
  if (!cuInitialized) {
    CUresult err = cuInit(0);
    if (err == CUDA_SUCCESS) {
      cuInitialized = true;
    }
    return err;
  }
  return CUDA_SUCCESS;
}

// Load a kernel function from PTX source for a specific GPU
// Returns the CUfunction handle, or NULL on failure
CUfunction artsLoadKernelFromPtx(const char *ptxSource, const char *kernelName,
                                 unsigned int gpuId) {
  if (!ptxSource || !kernelName) {
    ARTS_INFO("artsLoadKernelFromPtx: NULL ptxSource or kernelName\n");
    return NULL;
  }

  CUresult err = ensureCuInit();
  if (err != CUDA_SUCCESS) {
    ARTS_INFO("artsLoadKernelFromPtx: cuInit failed with error %d\n", err);
    return NULL;
  }

  // Compute content hash for cache lookup
  size_t ptxHash = hashPtxContent(ptxSource);

  std::lock_guard<std::mutex> lock(ptxCacheMutex);

  // Check if we have a cached module for this PTX content
  auto it = ptxModuleCache.find(ptxHash);
  CUmodule module = NULL;

  if (it != ptxModuleCache.end()) {
    // Found cached entry - check if module is loaded for this GPU
    if (gpuId < it->second.size() && it->second[gpuId] != NULL) {
      module = it->second[gpuId];
    }
  } else {
    // Create new entry in cache
    ptxModuleCache[ptxHash] = std::vector<CUmodule>(artsNodeInfo.gpu, NULL);
    it = ptxModuleCache.find(ptxHash);
  }

  if (module == NULL) {
    // Need to load module for this GPU
    // Get the CUDA context for this GPU
    CUdevice device;
    CUcontext ctx;

    err = cuDeviceGet(&device, gpuId);
    if (err != CUDA_SUCCESS) {
      ARTS_INFO("artsLoadKernelFromPtx: cuDeviceGet failed for GPU %u: %d\n",
                gpuId, err);
      return NULL;
    }

    // Create or get context for this device
    // Note: cudaSetDevice already creates a primary context, we can use it
    err = cuDevicePrimaryCtxRetain(&ctx, device);
    if (err != CUDA_SUCCESS) {
      ARTS_INFO("artsLoadKernelFromPtx: cuDevicePrimaryCtxRetain failed: %d\n",
                err);
      return NULL;
    }

    err = cuCtxSetCurrent(ctx);
    if (err != CUDA_SUCCESS) {
      ARTS_INFO("artsLoadKernelFromPtx: cuCtxSetCurrent failed: %d\n", err);
      cuDevicePrimaryCtxRelease(device);
      return NULL;
    }

    // Load the PTX module
    err = cuModuleLoadData(&module, ptxSource);
    if (err != CUDA_SUCCESS) {
      const char* errName;
      cuGetErrorName(err, &errName);
      ARTS_INFO("artsLoadKernelFromPtx: cuModuleLoadData failed: %s (%d)\n",
                errName ? errName : "unknown", err);
      cuDevicePrimaryCtxRelease(device);
      return NULL;
    }

    // Cache the module
    it->second[gpuId] = module;
    ARTS_INFO("artsLoadKernelFromPtx: Loaded PTX module for GPU %u\n", gpuId);
  }

  // Get the function from the module
  CUfunction func;
  err = cuModuleGetFunction(&func, module, kernelName);
  if (err != CUDA_SUCCESS) {
    const char* errName;
    cuGetErrorName(err, &errName);
    ARTS_INFO("artsLoadKernelFromPtx: cuModuleGetFunction(%s) failed: %s (%d)\n",
              kernelName, errName ? errName : "unknown", err);
    return NULL;
  }

  ARTS_INFO("artsLoadKernelFromPtx: Got function %s from PTX\n", kernelName);
  return func;
}

// Cleanup all cached PTX modules (call at shutdown)
void artsCleanupPtxModules() {
  std::lock_guard<std::mutex> lock(ptxCacheMutex);

  for (auto& entry : ptxModuleCache) {
    for (unsigned int i = 0; i < entry.second.size(); i++) {
      if (entry.second[i] != NULL) {
        CUdevice device;
        if (cuDeviceGet(&device, i) == CUDA_SUCCESS) {
          CUcontext ctx;
          if (cuDevicePrimaryCtxRetain(&ctx, device) == CUDA_SUCCESS) {
            cuCtxSetCurrent(ctx);
            cuModuleUnload(entry.second[i]);
            cuDevicePrimaryCtxRelease(device);
          }
        }
      }
    }
  }
  ptxModuleCache.clear();
}

//=============================================================================
// PTX-based GPU EDT Creation
//=============================================================================

artsGuid_t internalEdtCreateGpuPtx(const char *ptxSource, const char *kernelName,
                                   artsGuid_t *guid, unsigned int route,
                                   uint32_t paramc, uint64_t *paramv,
                                   uint32_t depc, dim3 grid, dim3 block,
                                   artsGuid_t endGuid, uint32_t slot,
                                   artsGuid_t dataGuid, bool hasDepv,
                                   int gpuToRunOn) {
  if (!ptxSource || !kernelName) {
    ARTS_INFO("Creating PTX GPU EDT: missing PTX or kernel name\n");
    return NULL_GUID;
  }
  if (paramc && !paramv) {
    ARTS_INFO("Creating PTX GPU EDT: paramv is null (paramc=%u)\n", paramc);
    return NULL_GUID;
  }
  ARTS_INFO("Creating PTX GPU EDT: kernel=%p, grid=(%d,%d,%d), block=(%d,%d,%d), route=%u, depc=%u\n",
            kernelName, grid.x, grid.y, grid.z, block.x, block.y, block.z, route, depc);

  unsigned int depSpace = (hasDepv) ? depc * sizeof(artsEdtDep_t) : 0;
  unsigned int edtSpace =
      sizeof(artsGpuEdt_t) + paramc * sizeof(uint64_t) + depSpace;

  artsGpuEdt_t *edt = (artsGpuEdt_t *)artsCallocAlignWithType(
      1, edtSpace, 16, artsEdtMemorySize);
  if (!edt) {
    ARTS_INFO("Creating PTX GPU EDT: allocation failed (size=%u)\n", edtSpace);
    return NULL_GUID;
  }
  edt->wrapperEdt.invalidateCount = 1;
  edt->grid = grid;
  edt->block = block;
  edt->gpuToRunOn = gpuToRunOn;
  edt->endGuid = endGuid;
  edt->slot = slot;
  edt->dataGuid = dataGuid;
  edt->passthrough = false;
  edt->lib = false;
  // Set PTX source and kernel name
  edt->ptxSource = ptxSource;
  edt->kernelName = kernelName;

  bool created = artsEdtCreateInternal(
      (struct artsEdt *)edt, ARTS_GPU_EDT, guid, route,
      artsThreadInfo.clusterId, edtSpace, NULL_GUID, NULL, paramc, paramv,
      depc, true, NULL_GUID, hasDepv, 0);

  ARTS_INFO("PTX GPU EDT created: guid=%lu, created=%d, depc=%u, hasDepv=%d\n",
            *guid, created, depc, hasDepv);

  return *guid;
}

artsGuid_t artsEdtCreateGpuPtx(const char *ptxSource, const char *kernelName,
                               unsigned int route, uint32_t paramc,
                               uint64_t *paramv, uint32_t depc, dim3 grid,
                               dim3 block, artsGuid_t endGuid, uint32_t slot,
                               artsGuid_t dataGuid) {
  artsGuid_t guid = NULL_GUID;
  return internalEdtCreateGpuPtx(ptxSource, kernelName, &guid, route, paramc,
                                 paramv, depc, grid, block, endGuid, slot,
                                 dataGuid, true, -1);
}

void artsRunGpu(void *edtPacket, artsGpu_t *artsGpu) {
  artsGpuEdt_t *edt = (artsGpuEdt_t *)edtPacket;
  artsEdt_t func = edt->wrapperEdt.funcPtr;
  uint32_t paramc = edt->wrapperEdt.paramc;
  uint32_t depc = edt->wrapperEdt.depc;
  uint64_t *paramv = (uint64_t *)(edt + 1);
  artsEdtDep_t *depv = (artsEdtDep_t *)(paramv + paramc);

  ARTS_INFO("artsRunGpu: guid=%lu, func=%p, paramc=%u, depc=%u, lib=%d\n",
            edt->wrapperEdt.currentEdt, func, paramc, depc, edt->lib);

  artsCudaSetDevice(artsGpu->device, true);

  if (artsNodeInfo.runGpuGcPreEdt) {
    // ARTS_INFO("Running Pre Edt GPU GC: %u\n", artsGpu->device);
    uint64_t freeMemSize = artsGpuCleanUpRouteTable(
        (unsigned int)-1, artsNodeInfo.deleteZerosGpuGc,
        (unsigned int)artsGpu->device);
    artsAtomicAddU64(&artsGpu->availGlobalMem, freeMemSize);
    artsAtomicAddU64(&freeBytes, freeMemSize);
  }

  artsAtomicAdd(&artsGpu->runningEdts, 1U);

  ARTS_INFO("artsRunGpu: prepDbs and scheduling to GPU\n");
  prepDbs(depc, depv, true);
  artsScheduleToGpu(func, paramc, paramv, depc, depv, edtPacket, artsGpu);
  ARTS_INFO("artsRunGpu: scheduled to GPU, returning\n");

  artsCudaRestoreDevice();
}

void artsGpuHostWrapUp(void *edtPacket, artsGuid_t toSignal, uint32_t slot,
                       artsGuid_t dataGuid) {
  ARTS_INFO("artsGpuHostWrapUp called: edtPacket=%p, toSignal=%lu, slot=%u, dataGuid=%lu\n",
            edtPacket, toSignal, slot, dataGuid);

  artsGpuEdt_t *edt = (artsGpuEdt_t *)edtPacket;
  uint32_t paramc = edt->wrapperEdt.paramc;
  uint32_t depc = edt->wrapperEdt.depc;
  uint64_t *paramv = (uint64_t *)(edt + 1);
  artsEdtDep_t *depv = (artsEdtDep_t *)(paramv + paramc);

  if (edt->lib) {
    ARTS_INFO("artsGpuHostWrapUp: lib EDT, firing OO\n");
    edt->wrapperEdt.invalidateCount = 0;
    artsRouteTableFireOO(edt->wrapperEdt.currentEdt, artsOutOfOrderHandler);
  } else if (edt->wrapperEdt.epochGuid) {
    ARTS_INFO("artsGpuHostWrapUp: incrementing epoch %lu\n", edt->wrapperEdt.epochGuid);
    incrementFinishedEpoch(edt->wrapperEdt.epochGuid);
  }

  ARTS_INFO("TO SIGNAL: %lu -> %lu slot: %u\n", toSignal, dataGuid, slot);
  // Signal next BEFORE releasing DBs so dependent EDTs can acquire them
  if (toSignal) {
    if (edt->passthrough)
      artsSignalEdt(toSignal, slot, depv[dataGuid].guid);
    else {
      artsType_t mode = artsGuidGetType(toSignal);
      if (mode == ARTS_EDT || mode == ARTS_GPU_EDT)
        artsSignalEdt(toSignal, slot, dataGuid);
      if (mode == ARTS_EVENT)
        artsEventSatisfySlot(toSignal, dataGuid, slot);
      if (mode ==
          ARTS_BUFFER) // This is for us to be able to block in a host edt
        artsSetBuffer(toSignal, 0, 0);
      if (mode == ARTS_PERSISTENT_EVENT)
        artsPersistentEventSatisfy(toSignal, slot, true);
    }
  }

  // Release DBs AFTER signaling so dependent EDTs can acquire them
  ARTS_INFO("artsGpuHostWrapUp: releasing DBs, edt->lib=%d\n", edt->lib);
  releaseDbs(depc, depv, true);

  artsEdtDelete((struct artsEdt *)edtPacket);
}

struct artsEdt *artsRuntimeStealGpuTask() {
  struct artsEdt *edt = NULL;
  if (artsNodeInfo.totalThreadCount > 1) {
    long unsigned int stealLoc;
    do {
      stealLoc = jrand48(artsThreadInfo.drand_buf);
      stealLoc = stealLoc % artsNodeInfo.totalThreadCount;
    } while (stealLoc == artsThreadInfo.threadId);
    edt = (struct artsEdt *)artsDequePopBack(artsNodeInfo.gpuDeque[stealLoc]);
  }
  return edt;
}

bool artsGpuSchedulerLoop() {
  static __thread bool logged = false;
  if (!logged) {
    ARTS_INFO("GPU Scheduler Loop active on thread %u\n", artsThreadInfo.threadId);
    logged = true;
  }
  artsGpu_t *artsGpu = NULL;
  artsHandleNewEdts();

  struct artsEdt *edtFound = (struct artsEdt *)NULL;
  if (!(edtFound =
            (struct artsEdt *)artsDequePopFront(artsThreadInfo.myGpuDeque))) {
    if (!edtFound)
      edtFound = artsRuntimeStealGpuTask();
  }

  bool ranGpuEdt = false;
  if (edtFound) {
    ARTS_INFO("GPU Scheduler found EDT: guid=%lu, type=%d\n", edtFound->currentEdt, edtFound->header.type);
    artsGpu = artsFindGpu(edtFound);
    if (artsGpu) {
      artsRunGpu(edtFound, artsGpu);
      ranGpuEdt = true;
    } else
      artsDequePushFront(artsThreadInfo.myGpuDeque, edtFound, 0);
  }

  if (!ranGpuEdt)
    checkStreams(artsNodeInfo.gpuBuffOn);

  bool ranCpuEdt = artsDefaultSchedulerLoop();
  if (artsNodeInfo.runGpuGcIdle && !ranGpuEdt && !ranCpuEdt) {
    long unsigned int gpuId = jrand48(artsThreadInfo.drand_buf);
    gpuId = gpuId % artsNodeInfo.gpu;
    artsGpu = &artsGpus[gpuId];
    ARTS_DEBUG("Running Idle GPU GC: %u\n", gpuId);
    artsCudaSetDevice(artsGpu->device, true);

    uint64_t freeMemSize = artsGpuCleanUpRouteTable(
        (unsigned int)-1, artsNodeInfo.deleteZerosGpuGc,
        (unsigned int)artsGpu->device);
    artsAtomicAddU64(&artsGpu->availGlobalMem, freeMemSize);
    artsAtomicAddU64(&freeBytes, freeMemSize);

    artsCudaRestoreDevice();
  }

  return ranCpuEdt;
}

#define GCHARDLIMIT 2000000000000
__thread uint64_t backoff = 1;
__thread uint64_t gcCounter = 0;

bool artsGpuSchedulerBackoffLoop() {
  artsGpu_t *artsGpu = NULL;
  artsHandleNewEdts();

  struct artsEdt *edtFound = (struct artsEdt *)NULL;
  if (!(edtFound =
            (struct artsEdt *)artsDequePopFront(artsThreadInfo.myGpuDeque))) {
    if (!edtFound)
      edtFound = artsRuntimeStealGpuTask();
  }

  bool ranGpuEdt = false;
  if (edtFound) {
    artsGpu = artsFindGpu(edtFound);
    if (artsGpu) {
      artsRunGpu(edtFound, artsGpu);
      ranGpuEdt = true;
    } else
      artsDequePushFront(artsThreadInfo.myGpuDeque, edtFound, 0);
  }

  if (!ranGpuEdt)
    checkStreams(artsNodeInfo.gpuBuffOn);

  bool ranCpuEdt = artsDefaultSchedulerLoop();

  if (ranCpuEdt || ranGpuEdt)
    backoff = 1;

  if (!ranGpuEdt && !ranCpuEdt) {
    if (artsNodeInfo.runGpuGcIdle && gcCounter % backoff == 0) {
      long unsigned int gpuId = jrand48(artsThreadInfo.drand_buf);
      gpuId = gpuId % artsNodeInfo.gpu;
      artsGpu = &artsGpus[gpuId];
      ARTS_DEBUG("Running Idle GPU GC: %u\n", gpuId);
      artsCudaSetDevice(artsGpu->device, true);

      uint64_t freeMemSize = artsGpuCleanUpRouteTable(
          (unsigned int)-1, artsNodeInfo.deleteZerosGpuGc,
          (unsigned int)artsGpu->device);
      artsAtomicAddU64(&artsGpu->availGlobalMem, freeMemSize);
      artsAtomicAddU64(&freeBytes, freeMemSize);

      artsCudaRestoreDevice();

      if (backoff < GCHARDLIMIT)
        backoff *= 32;
      if (!backoff)
        backoff = 1;
      ARTS_DEBUG("Backoff: %u\n", backoff);
    }
    gcCounter++;
  }

  return ranCpuEdt;
}

// Global atomic GC flag - stores gpuId + 1 (0 means no GC needed)
extern volatile unsigned int globalRunGCFlag;

bool artsGpuSchedulerDemandLoop() {
  static __thread bool logged = false;
  if (!logged) {
    ARTS_INFO("GPU Scheduler Demand Loop active on thread %u\n", artsThreadInfo.threadId);
    logged = true;
  }
  artsGpu_t *artsGpu = NULL;
  artsHandleNewEdts();

  struct artsEdt *edtFound = (struct artsEdt *)NULL;
  if (!(edtFound =
            (struct artsEdt *)artsDequePopFront(artsThreadInfo.myGpuDeque))) {
    if (!edtFound)
      edtFound = artsRuntimeStealGpuTask();
  }

  bool ranGpuEdt = false;
  if (edtFound) {
    ARTS_INFO("GPU Scheduler Demand found EDT: guid=%lu, type=%d\n", edtFound->currentEdt, edtFound->header.type);
    artsGpu = artsFindGpu(edtFound);
    if (artsGpu) {
      ARTS_INFO("Running GPU EDT on GPU device %d\n", artsGpu->device);
      artsRunGpu(edtFound, artsGpu);
      ranGpuEdt = true;
    } else {
      ARTS_INFO("No GPU available, re-queueing EDT\n");
      artsDequePushFront(artsThreadInfo.myGpuDeque, edtFound, 0);
    }
  }

  if (!ranGpuEdt)
    checkStreams(artsNodeInfo.gpuBuffOn);

  bool ranCpuEdt = artsDefaultSchedulerLoop();

  if (!ranGpuEdt && !ranCpuEdt) {
    // Use atomic fetch-and-swap to atomically read and clear the GC flag
    unsigned int gcFlag = artsAtomicSwap(&globalRunGCFlag, 0U);
    if (artsNodeInfo.runGpuGcIdle && gcFlag) {
      long unsigned int gpuId = gcFlag - 1;

      artsGpu = &artsGpus[gpuId];
      ARTS_DEBUG("Running Idle GPU GC: %u\n", gpuId);
      artsCudaSetDevice(artsGpu->device, true);

      uint64_t freeMemSize = artsGpuCleanUpRouteTable(
          (unsigned int)-1, artsNodeInfo.deleteZerosGpuGc,
          (unsigned int)artsGpu->device);
      artsAtomicAddU64(&artsGpu->availGlobalMem, freeMemSize);
      artsAtomicAddU64(&freeBytes, freeMemSize);

      artsCudaRestoreDevice();
    }
  }

  return ranCpuEdt;
}

void artsPutInDbFromGpu(void *ptr, artsGuid_t dbGuid, unsigned int offset,
                        unsigned int size, bool freeData) {
  unsigned int rank = artsGuidGetRank(dbGuid);
  if (rank == artsGlobalRankId) {
    struct artsDb *db = (struct artsDb *)artsRouteTableLookupItem(dbGuid);
    if (db) {
      void *data = (void *)(((char *)(db + 1)) + offset);
      // memcpy(data, ptr, size);
      CHECKCORRECT(cudaMemcpyAsync(data, ptr, size, cudaMemcpyDeviceToHost,
                                   *artsLocalStream));

    } else {
      void *cpyPtr = artsMalloc(size);
      // memcpy(cpyPtr, ptr, size);
      CHECKCORRECT(cudaMemcpyAsync(cpyPtr, ptr, size, cudaMemcpyDeviceToHost,
                                   *artsLocalStream));
      artsOutOfOrderPutInDb(cpyPtr, NULL_GUID, dbGuid, 0, offset, size,
                            NULL_GUID);
    }
    if (freeData)
      artsGpuRouteTableAddItemToDeleteRace(ptr, 0, dbGuid, artsLocalGpuId);
  }
}

artsLCSyncFunction_t lcSyncFunction[] = {
    artsMemcpyGpuDb,         artsGetLatestGpuDb,
    artsGetRandomGpuDb,      artsGetNonZerosUnsignedInt,
    artsGetMinDbUnsignedInt, artsAddDbUnsignedInt,
    artsXorDbUint64};

artsLCSyncFunctionGpu_t lcSyncFunctionGpu[] = {
    artsCopyGpuDb,           artsCopyGpuDb,
    artsCopyGpuDb,           artsNonZeroGpuDbUnsignedInt,
    artsMinGpuDbUnsignedInt, artsAddGpuDbUnsignedInt,
    artsXorGpuDbUint64};

unsigned int lcSyncElementSize[] = {sizeof(unsigned int), sizeof(unsigned int),
                                    sizeof(unsigned int), sizeof(unsigned int),
                                    sizeof(unsigned int), sizeof(unsigned int),
                                    sizeof(uint64_t)};

void internalLCSyncGPU(artsGuid_t acqGuid, struct artsDb *db) {
  if (db) {
    artsLCMeta_t host;
    artsLCMeta_t dev;
    host.guid = acqGuid;
    host.data = (void *)(db + 1);
    host.dataSize = db->header.size - sizeof(struct artsDb);
    host.hostVersion = &db->version;
    host.hostTimeStamp = &db->timeStamp;
    host.gpuVersion = 0;
    host.gpuTimeStamp = 0;
    host.gpu = -1;
    host.readLock = &db->reader;
    host.writeLock = &db->writer;

    artsCudaSetDevice(-1, true);

    bool copyOnly = false;
    unsigned int size = db->header.size;
    struct artsDb *tempSpace = (struct artsDb *)artsMallocAlign(size, 16);

    gpuGCWriteLock(); // Don't let the gc take our copies...
    ARTS_DEBUG("FUNCTION: %u\n", artsNodeInfo.gpuLCSync);
    unsigned int remMask = gpuLCReduce(
        acqGuid, db, lcSyncFunctionGpu[artsNodeInfo.gpuLCSync], &copyOnly);
    ARTS_DEBUG("RemMask: %u\n", remMask);
    for (int i = 0; i < artsNodeInfo.gpu; i++) {
      if (remMask & (1 << i)) {
        ARTS_DEBUG("Merging: %u\n", i);
        unsigned int gpuVersion;
        unsigned int timeStamp;
        void *dataPtr = artsGpuRouteTableLookupDbRes(acqGuid, i, &gpuVersion,
                                                     &timeStamp, false);
        if (dataPtr) {
          if (!copyOnly)
            artsGpuInvalidateOnRouteTable(acqGuid, i);

          artsCudaSetDevice(i, false);
          getDataFromStreamNow(i, tempSpace, dataPtr, size, false);
          artsGpuRouteTableReturnDb(acqGuid, !copyOnly, i);

          dev.guid = acqGuid;
          dev.data = (void *)(tempSpace + 1);
          dev.dataSize = tempSpace->header.size - sizeof(struct artsDb);
          dev.hostVersion = &tempSpace->version;
          dev.hostTimeStamp = &tempSpace->timeStamp;
          dev.gpuVersion = gpuVersion;
          dev.gpuTimeStamp = timeStamp;
          dev.gpu = i;
          dev.readLock = NULL;
          dev.writeLock = NULL;
          if (copyOnly)
            lcSyncFunction[0](&host, &dev);
          else
            lcSyncFunction[artsNodeInfo.gpuLCSync](&host, &dev);

          artsMetricsTriggerEvent(artsGpuSync, artsThread, 1);
        }
      } else {
        ARTS_DEBUG("NO DB COPY ON GPU %d\n", i);
      }
    }
    gpuGCWriteUnlock();
    artsFree(tempSpace);
    artsCudaRestoreDevice();
  }
}

void internalLCSyncCPU(artsGuid_t acqGuid, struct artsDb *db) {

  // struct artsDb * db = (struct artsDb*) artsRouteTableLookupItem(acqGuid);
  if (db) {
    artsLCMeta_t host;
    artsLCMeta_t dev;
    host.guid = acqGuid;
    host.data = (void *)(db + 1);
    host.dataSize = db->header.size - sizeof(struct artsDb);
    host.hostVersion = &db->version;
    host.hostTimeStamp = &db->timeStamp;
    host.gpuVersion = 0;
    host.gpuTimeStamp = 0;
    host.gpu = -1;
    host.readLock = &db->reader;
    host.writeLock = &db->writer;

    // artsCudaSetDevice(-1, true);

    unsigned int size = db->header.size;
    struct artsDb *tempSpace = (struct artsDb *)artsMallocAlign(size, 16);
    gpuGCWriteLock(); // Don't let the gc take our copies...
    for (int i = 0; i < artsNodeInfo.gpu; i++) {
      unsigned int gpuVersion;
      unsigned int timeStamp;
      ARTS_DEBUG("acqGuid: %lu type: %u i: %u\n", acqGuid,
                 artsGuidGetType(acqGuid), i);
      void *dataPtr =
          artsGpuRouteTableLookupDb(acqGuid, i, &gpuVersion, &timeStamp);
      if (dataPtr) {
        ARTS_DEBUG("i: %u %lu\n", i, acqGuid);
        artsGpuInvalidateOnRouteTable(acqGuid, i);
        // artsCudaSetDevice(i, false);

        // artsCudaMemCpyFromDev(tempSpace, dataPtr, size);
        getDataFromStreamNow(i, tempSpace, dataPtr, size, false);
        artsGpuRouteTableReturnDb(acqGuid, true, i);

        dev.guid = acqGuid;
        dev.data = (void *)(tempSpace + 1);
        dev.dataSize = tempSpace->header.size - sizeof(struct artsDb);
        dev.hostVersion = &tempSpace->version;
        dev.hostTimeStamp = &tempSpace->timeStamp;
        dev.gpuVersion = gpuVersion;
        dev.gpuTimeStamp = timeStamp;
        dev.gpu = i;
        dev.readLock = NULL;
        dev.writeLock = NULL;
        lcSyncFunction[artsNodeInfo.gpuLCSync](&host, &dev);
      } else {
        ARTS_DEBUG("NO DB COPY ON GPU %d\n", i);
      }
    }
    gpuGCWriteUnlock();
    artsFree(tempSpace);
    // artsCudaRestoreDevice();
  }
}
