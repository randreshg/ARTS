/******************************************************************************
** Stub implementations for ARTS runtime functions
** These are placeholder implementations that allow the core library to build
** Users should provide their own implementations in their application code
******************************************************************************/

#include "arts/arts.h"

// Stub implementation for artsMain - users should override this
void artsMain(void) {
    // This function is expected to be provided by the user
    // It should contain the main application logic
}

// Stub implementation for initPerNode - users should override this
void initPerNode(unsigned int nodeId, int argc, char** argv) {
    // This function is expected to be provided by the user
    // It should contain per-node initialization logic
    (void)nodeId;
    (void)argc;
    (void)argv;
}

// Stub implementation for initPerWorker - users should override this
void initPerWorker(unsigned int nodeId, unsigned int workerId, int argc, char** argv) {
    // This function is expected to be provided by the user
    // It should contain per-worker initialization logic
    (void)nodeId;
    (void)workerId;
    (void)argc;
    (void)argv;
}

