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
#include "arts/gpu/GpuStreamBuffer.h"

#include <cuda.h>  // CUDA Driver API for cuLaunchKernel
#include <vector>

#include "arts/gpu/GpuRuntime.cuh"
#include "arts/introspection/Metrics.h"
#include "arts/runtime/Globals.h"
#include "arts/system/ArtsPrint.h"
#include "arts/utils/Atomics.h"

#define CHECKSTREAM 32
#define MAXSTREAM 32
#define MAXBUFFER 128

volatile unsigned int streamCheckCount[MAXSTREAM] = {0};

volatile unsigned int buffLock[MAXSTREAM] = {0};
unsigned int hostToDevCount[MAXSTREAM] = {0};
unsigned int kernelToDevCount[MAXSTREAM] = {0};
unsigned int devToHostCount[MAXSTREAM] = {0};
unsigned int wrapUpCount[MAXSTREAM] = {0};

artsBufferMemMove_t hostToDevBuff[MAXSTREAM][MAXBUFFER];
artsBufferKernel_t kernelToDevBuff[MAXSTREAM][MAXBUFFER];
artsBufferMemMove_t devToHostBuff[MAXSTREAM][MAXBUFFER];
void *wrapUpBuff[MAXSTREAM][MAXBUFFER];

void checkOccupancy(artsEdt_t fnPtr, unsigned int gpuId, dim3 block) {
  int maxActiveBlocks;
  int blockSize = (int)block.x * block.y * block.z;
  struct cudaDeviceProp prop = artsGpus[gpuId].prop;

  CHECKCORRECT(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &maxActiveBlocks, (const void *)fnPtr, (int)blockSize, 0));
  float occupancy = (maxActiveBlocks * blockSize / prop.warpSize) /
                    (float)(prop.maxThreadsPerMultiProcessor / prop.warpSize);

  // Moving average of occupancy
  artsLock(&artsGpus[gpuId].deviceLock);
  artsGpus[gpuId].occupancy = (occupancy + (artsGpus[gpuId].totalEdts - 1) *
                                               artsGpus[gpuId].occupancy) /
                              (++artsGpus[gpuId].totalEdts);
  artsUnlock(&artsGpus[gpuId].deviceLock);
}

bool pushDataToStream(unsigned int gpuId, void *dst, void *src, size_t count,
                      bool buff) {
  if (buff) {
    artsLock(&buffLock[gpuId]);
    hostToDevBuff[gpuId][hostToDevCount[gpuId]].dst = dst;
    hostToDevBuff[gpuId][hostToDevCount[gpuId]].src = src;
    hostToDevBuff[gpuId][hostToDevCount[gpuId]].count = count;
    hostToDevCount[gpuId]++;

    bool ret = false;
    if (hostToDevCount[gpuId] == MAXBUFFER)
      ret = flushStream(gpuId);
    artsUnlock(&buffLock[gpuId]);
    return ret;
  }

  // Use dedicated H2D stream for better overlap with compute
  cudaStream_t h2dStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_H2D];
  if (src) {
    CHECKCORRECT(cudaMemcpyAsync(dst, src, count, cudaMemcpyHostToDevice,
                                 h2dStream));
    artsMetricsTriggerEvent(artsGpuBWPush, artsThread, count);
  } else
    CHECKCORRECT(cudaMemsetAsync(dst, 0, count, h2dStream));
  return true;
}

bool getDataFromStream(unsigned int gpuId, void *dst, void *src, size_t count,
                       bool buff) {
  if (buff) {
    artsLock(&buffLock[gpuId]);
    devToHostBuff[gpuId][devToHostCount[gpuId]].dst = dst;
    devToHostBuff[gpuId][devToHostCount[gpuId]].src = src;
    devToHostBuff[gpuId][devToHostCount[gpuId]].count = count;
    devToHostCount[gpuId]++;

    bool ret = false;
    if (devToHostCount[gpuId] == MAXBUFFER)
      ret = flushStream(gpuId);
    artsUnlock(&buffLock[gpuId]);
    return ret;
  }
  // Use dedicated D2H stream for better overlap with compute
  cudaStream_t d2hStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_D2H];
  CHECKCORRECT(cudaMemcpyAsync(dst, src, count, cudaMemcpyDeviceToHost,
                               d2hStream));
  artsMetricsTriggerEvent(artsGpuBWPull, artsThread, count);
  return true;
}

bool pushKernelToStream(unsigned int gpuId, uint32_t paramc, uint64_t *paramv,
                        uint32_t depc, artsEdtDep_t *depv, artsEdt_t fnPtr,
                        dim3 grid, dim3 block, bool buff) {
  if (buff) {
    artsLock(&buffLock[gpuId]);
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].paramc = paramc;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].paramv = paramv;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].depc = depc;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].depv = depv;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].fnPtr = fnPtr;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].grid[0] = grid.x;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].grid[1] = grid.y;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].grid[2] = grid.z;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].block[0] = block.x;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].block[1] = block.y;
    kernelToDevBuff[gpuId][kernelToDevCount[gpuId]].block[2] = block.z;
    kernelToDevCount[gpuId]++;

    bool ret = false;
    if (kernelToDevCount[gpuId] == MAXBUFFER)
      ret = flushStream(gpuId);
    artsUnlock(&buffLock[gpuId]);
    return ret;
  }

  // Use dedicated compute stream
  cudaStream_t computeStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_COMPUTE];

  // Wait for H2D transfers to complete before starting compute
  CHECKCORRECT(cudaStreamWaitEvent(computeStream, artsGpus[gpuId].h2dDoneEvent, 0));

  void *kernelArgs[] = {&paramc, &paramv, &depc, &depv};
  CHECKCORRECT(cudaLaunchKernel((const void *)fnPtr, grid, block,
                                (void **)kernelArgs, (size_t)0,
                                computeStream));
  checkOccupancy(fnPtr, gpuId, block);

  // Record event when compute completes for D2H synchronization
  CHECKCORRECT(cudaEventRecord(artsGpus[gpuId].computeDoneEvent, computeStream));

  artsMetricsTriggerEvent(artsGpuEdt, artsThread, 1);
  return true;
}

// Signal H2D transfers are complete - call after all H2D data is pushed
void signalH2DComplete(unsigned int gpuId) {
  cudaStream_t h2dStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_H2D];
  CHECKCORRECT(cudaEventRecord(artsGpus[gpuId].h2dDoneEvent, h2dStream));
}

// Wait for compute to complete before D2H - call before D2H transfers
void waitForComputeComplete(unsigned int gpuId) {
  cudaStream_t d2hStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_D2H];
  CHECKCORRECT(cudaStreamWaitEvent(d2hStream, artsGpus[gpuId].computeDoneEvent, 0));
}

// PTX kernel launch using CUDA Driver API
// Note: PTX kernels don't support buffering - they are launched immediately
bool pushPtxKernelToStream(unsigned int gpuId, uint32_t paramc, uint64_t *paramv,
                           uint32_t depc, artsEdtDep_t *depv, CUfunction cuFunc,
                           dim3 grid, dim3 block) {
  if (!cuFunc) {
    ARTS_INFO("pushPtxKernelToStream: NULL CUfunction\n");
    return false;
  }

  // Use dedicated compute stream
  cudaStream_t computeStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_COMPUTE];

  // Wait for H2D transfers to complete before starting compute
  CHECKCORRECT(cudaStreamWaitEvent(computeStream, artsGpus[gpuId].h2dDoneEvent, 0));

  // Get the CUDA stream as CUstream
  // cudaStream_t is compatible with CUstream
  CUstream cuStream = (CUstream)computeStream;

  // Set up kernel arguments
  // PTX kernels use dependency DB pointers first, then captures from paramv.
  std::vector<void *> kernelArgs;
  kernelArgs.reserve(paramc);
  for (uint32_t i = 0; i < depc; ++i)
    kernelArgs.push_back(&depv[i].ptr);
  for (uint32_t i = depc; i < paramc; ++i)
    kernelArgs.push_back(&paramv[i]);

  ARTS_INFO("pushPtxKernelToStream: launching on GPU %u, grid=(%u,%u,%u), block=(%u,%u,%u)\n",
            gpuId, grid.x, grid.y, grid.z, block.x, block.y, block.z);

  CUresult err = cuLaunchKernel(
      cuFunc,
      grid.x, grid.y, grid.z,     // Grid dimensions
      block.x, block.y, block.z,  // Block dimensions
      0,                          // Shared memory bytes
      cuStream,                   // Stream
      kernelArgs.empty() ? nullptr : kernelArgs.data(), // Kernel arguments
      NULL                        // Extra (unused)
  );

  if (err != CUDA_SUCCESS) {
    const char* errName;
    cuGetErrorName(err, &errName);
    ARTS_INFO("pushPtxKernelToStream: cuLaunchKernel failed: %s (%d)\n",
              errName ? errName : "unknown", err);
    return false;
  }

  // Record event when compute completes for D2H synchronization
  CHECKCORRECT(cudaEventRecord(artsGpus[gpuId].computeDoneEvent, computeStream));

  artsMetricsTriggerEvent(artsGpuEdt, artsThread, 1);
  ARTS_INFO("pushPtxKernelToStream: kernel launched successfully\n");
  return true;
}

bool pushWrapUpToStream(unsigned int gpuId, void *hostClosure, bool buff) {
  if (buff) {
    artsLock(&buffLock[gpuId]);
    wrapUpBuff[gpuId][wrapUpCount[gpuId]] = hostClosure;
    wrapUpCount[gpuId]++;

    bool ret = false;
    if (wrapUpCount[gpuId] == MAXBUFFER)
      ret = flushStream(gpuId);
    artsUnlock(&buffLock[gpuId]);
    return ret;
  }

  // Use D2H stream for wrapup since it runs after D2H transfers complete
  cudaStream_t d2hStream = artsGpus[gpuId].streams[ARTS_GPU_STREAM_D2H];
#if CUDART_VERSION >= 10000
  CHECKCORRECT(cudaLaunchHostFunc(d2hStream, artsWrapUpHostFunc,
                                  hostClosure));
#else
  CHECKCORRECT(cudaStreamAddCallback(d2hStream, artsWrapUp,
                                     hostClosure, 0));
#endif
  return true;
}

bool flushMemStream(unsigned int gpuId, unsigned int *count,
                    artsBufferMemMove_t *buff, enum cudaMemcpyKind kind) {
  unsigned int max = *count;
  if (max > 0) {
    uint64_t dataSize = 0;
    for (unsigned int i = 0; i < max; i++) {
      if (buff[i].src) {
        // ARTS_INFO("i: %u %p %p %u %p\n", i, buff[i].dst, buff[i].src,
        // buff[i].count,  &artsGpus[gpuId].stream);
        CHECKCORRECT(cudaMemcpyAsync(buff[i].dst, buff[i].src, buff[i].count,
                                     kind, artsGpus[gpuId].stream));
        dataSize += buff[i].count;
      } else
        CHECKCORRECT(cudaMemsetAsync(buff[i].dst, 0, buff[i].count,
                                     artsGpus[gpuId].stream));
    }
    *count = 0;
    if (kind == cudaMemcpyHostToDevice) {
      artsMetricsTriggerEvent(artsGpuBWPush, artsThread, dataSize);
    } else {
      artsMetricsTriggerEvent(artsGpuBWPull, artsThread, dataSize);
    }
    return true;
  }
  return false;
}

bool flushKernelStream(unsigned int gpuId) {
  bool ret = (kernelToDevCount[gpuId] > 0);
  if (ret) {
    for (unsigned int i = 0; i < kernelToDevCount[gpuId]; i++) {
      void *kernelArgs[] = {
          &kernelToDevBuff[gpuId][i].paramc, &kernelToDevBuff[gpuId][i].paramv,
          &kernelToDevBuff[gpuId][i].depc, &kernelToDevBuff[gpuId][i].depv};
      dim3 grid(kernelToDevBuff[gpuId][i].grid[0],
                kernelToDevBuff[gpuId][i].grid[1],
                kernelToDevBuff[gpuId][i].grid[2]);
      dim3 block(kernelToDevBuff[gpuId][i].block[0],
                 kernelToDevBuff[gpuId][i].block[1],
                 kernelToDevBuff[gpuId][i].block[2]);
      CHECKCORRECT(cudaLaunchKernel(
          (const void *)kernelToDevBuff[gpuId][i].fnPtr, grid, block,
          (void **)kernelArgs, (size_t)0, artsGpus[gpuId].stream));
      checkOccupancy(kernelToDevBuff[gpuId][i].fnPtr, gpuId, block);
    }
    artsMetricsTriggerEvent(artsGpuEdt, artsThread, kernelToDevCount[gpuId]);
    kernelToDevCount[gpuId] = 0;
  }
  return ret;
}

bool flushWrapUpStream(unsigned int gpuId) {
  bool ret = (wrapUpCount[gpuId] > 0);
  for (unsigned int i = 0; i < wrapUpCount[gpuId]; i++) {
#if CUDART_VERSION >= 10000
    CHECKCORRECT(cudaLaunchHostFunc(artsGpus[gpuId].stream, artsWrapUpHostFunc,
                                    wrapUpBuff[gpuId][i]));
#else
    CHECKCORRECT(cudaStreamAddCallback(artsGpus[gpuId].stream, artsWrapUp,
                                       wrapUpBuff[gpuId][i], 0));
#endif
  }
  wrapUpCount[gpuId] = 0;
  return ret;
}

bool flushStream(unsigned int gpuId) {
  ARTS_DEBUG("%u %u %u %u\n", hostToDevCount[gpuId], kernelToDevCount[gpuId],
             devToHostCount[gpuId], wrapUpCount[gpuId]);
  if (hostToDevCount[gpuId] || kernelToDevCount[gpuId] ||
      devToHostCount[gpuId] || wrapUpCount[gpuId]) {
    artsCudaSetDevice(gpuId, true);

    flushMemStream(gpuId, &hostToDevCount[gpuId], hostToDevBuff[gpuId],
                   cudaMemcpyHostToDevice);
    flushKernelStream(gpuId);
    flushMemStream(gpuId, &devToHostCount[gpuId], devToHostBuff[gpuId],
                   cudaMemcpyDeviceToHost);
    flushWrapUpStream(gpuId);

    artsCudaRestoreDevice();
    artsMetricsTriggerEvent(artsGpuBufferFlush, artsThread, 1);
    return true;
  }
  return false;
}

void copyGputoGpu(void *dst, unsigned int dstGpuId, void *src,
                  unsigned int srcGpuId, unsigned int size) {
  // We need to lock in a fixed order, so smallest first
  unsigned int first = (dstGpuId < srcGpuId) ? dstGpuId : srcGpuId;
  unsigned int second = (dstGpuId == first) ? srcGpuId : dstGpuId;
  artsLock(&buffLock[first]);
  artsLock(&buffLock[second]);

  // Flush the streams to make sure everything is done
  flushStream(dstGpuId);
  flushStream(srcGpuId);
  CHECKCORRECT(cudaStreamSynchronize(artsGpus[srcGpuId].stream));

  // Next lets move the data
  CHECKCORRECT(cudaMemcpyPeerAsync(dst, dstGpuId, src, srcGpuId, size,
                                   artsGpus[dstGpuId].stream));

  artsCudaRestoreDevice();

  // Unlock in the correct order
  artsUnlock(&buffLock[second]);
  artsUnlock(&buffLock[first]);
}

void doReductionNow(unsigned int gpuId, void *sink, void *src,
                    artsLCSyncFunctionGpu_t fnPtr, unsigned int elementSize,
                    unsigned int size) {
  artsLock(&buffLock[gpuId]);
  flushStream(gpuId);

  artsCudaSetDevice(gpuId, true);
  size -= sizeof(struct artsDb);

  // Next lets run the reduce function on the dbData and the shadow copy (dst)
  unsigned int tileSize = size / elementSize;
  ARTS_INFO("TileSize: %u\n", tileSize);
  if (tileSize < 32) {
    dim3 block(tileSize, 1, 1); // For volta...
    dim3 grid(1, 1, 1);
    void *kernelArgs[] = {&sink, &src};
    ARTS_INFO("SRC: %p DST: %p\n", sink, src);
    CHECKCORRECT(cudaLaunchKernel((const void *)fnPtr, grid, block,
                                  (void **)kernelArgs, (size_t)0,
                                  artsGpus[gpuId].stream));
  } else {
    dim3 block(32, 1, 1); // For volta...
    dim3 grid((tileSize + 32 - 1) / 32, 1, 1);
    void *kernelArgs[] = {&sink, &src};
    CHECKCORRECT(cudaLaunchKernel((const void *)fnPtr, grid, block,
                                  (void **)kernelArgs, (size_t)0,
                                  artsGpus[gpuId].stream))
  }

  artsCudaRestoreDevice();
  artsUnlock(&buffLock[gpuId]);
}

void reduceDatafromGpus(void *dst, unsigned int dstGpuId, void *src,
                        unsigned int srcGpuId, unsigned int size,
                        artsLCSyncFunctionGpu_t fnPtr, unsigned int elementSize,
                        void *dbData) {
  ARTS_DEBUG("ELEMENT SIZE: %lu\n", elementSize);
  // We need to lock in a fixed order, so smallest first
  unsigned int first = (dstGpuId < srcGpuId) ? dstGpuId : srcGpuId;
  unsigned int second = (dstGpuId == first) ? srcGpuId : dstGpuId;
  artsLock(&buffLock[first]);
  artsLock(&buffLock[second]);

  // Flush the streams to make sure everything is done
  flushStream(dstGpuId);
  flushStream(srcGpuId);
  CHECKCORRECT(cudaStreamSynchronize(artsGpus[srcGpuId].stream));
  // I think we don't need to synchronize the destination stream since we are
  // just adding to it...
  //  CHECKCORRECT(cudaStreamSynchronize(artsGpus[dstGpuId].stream));

  // Next lets move the data
  CHECKCORRECT(cudaMemcpyPeerAsync(dst, dstGpuId, src, srcGpuId, size,
                                   artsGpus[dstGpuId].stream));

  artsCudaSetDevice(dstGpuId, true);

  // Lets remove the db header part
  size -= sizeof(struct artsDb);

  // Next lets run the reduce function on the dbData and the shadow copy (dst)
  unsigned int tileSize = size / elementSize;
  ARTS_DEBUG("TileSize: %u\n", tileSize);
  if (tileSize < 32) {
    dim3 block(tileSize, 1, 1); // For volta...
    dim3 grid(1, 1, 1);
    void *kernelArgs[] = {&dbData, &dst};
    ARTS_DEBUG("SRC: %p DST: %p\n", dbData, dst);
    CHECKCORRECT(cudaLaunchKernel((const void *)fnPtr, grid, block,
                                  (void **)kernelArgs, (size_t)0,
                                  artsGpus[dstGpuId].stream));
  } else {
    dim3 block(32, 1, 1); // For volta...
    dim3 grid((tileSize + 32 - 1) / 32, 1, 1);
    void *kernelArgs[] = {&dbData, &dst};
    ARTS_DEBUG("SRC: %p DST: %p\n", dbData, dst);
    CHECKCORRECT(cudaLaunchKernel((const void *)fnPtr, grid, block,
                                  (void **)kernelArgs, (size_t)0,
                                  artsGpus[dstGpuId].stream))
  }

  // CHECKCORRECT(cudaStreamSynchronize(artsGpus[srcGpuId].stream));
  // CHECKCORRECT(cudaStreamSynchronize(artsGpus[dstGpuId].stream));
  artsCudaRestoreDevice();

  // Unlock in the correct order
  artsUnlock(&buffLock[second]);
  artsUnlock(&buffLock[first]);
}

void getDataFromStreamNow(unsigned int gpuId, void *dst, void *src,
                          size_t count, bool buff) {
  if (buff) {
    artsLock(&buffLock[gpuId]);
    flushStream(gpuId);
    artsUnlock(&buffLock[gpuId]);
  }
  ARTS_DEBUG("GETTING[%u]: %p %p size: %u\n", gpuId, dst, src, count);
  CHECKCORRECT(cudaMemcpyAsync(dst, src, count, cudaMemcpyDeviceToHost,
                               artsGpus[gpuId].stream));
  CHECKCORRECT(cudaStreamSynchronize(artsGpus[gpuId].stream));
}

bool checkStreams(bool buffOn) {
  if (buffOn) {
    bool ret = false;
    for (unsigned int i = 0; i < artsNodeInfo.gpu; i++) {
      if (hostToDevCount[i] || kernelToDevCount[i] || devToHostCount[i] ||
          wrapUpCount[i]) {
        artsAtomicFetchAdd(&streamCheckCount[i], 1U);
        if (streamCheckCount[i] % CHECKSTREAM == 0) {
          artsLock(&buffLock[i]);
          ret |= flushStream(i);
          artsUnlock(&buffLock[i]);
        }
      }
    }
    return ret;
  }
  // When buffering is off, still need to poll streams to trigger callbacks
  // cudaLaunchHostFunc callbacks require host interaction to execute
  // Poll all streams (H2D, compute, D2H) for each GPU
  for (unsigned int i = 0; i < artsNodeInfo.gpu; i++) {
    for (int s = 0; s < ARTS_GPU_STREAM_COUNT; ++s) {
      cudaError_t status = cudaStreamQuery(artsGpus[i].streams[s]);
      // cudaStreamQuery returns cudaSuccess if stream is idle (all work done)
      // or cudaErrorNotReady if still busy. Either way, it triggers callbacks.
      (void)status; // Ignore the status, we just want to trigger callbacks
    }
  }
  return false;
}