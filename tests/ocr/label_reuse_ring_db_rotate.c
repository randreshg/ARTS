/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* label_reuse_ring_db_rotate — label_reuse_ring_db with the creator rotating
 * over every rank, the home included, and each generation read on the rank
 * that creates the next one.  That rank therefore holds a reader's cache of
 * generation g when it creates generation g+1: the create must wait behind
 * that cache until generation g's teardown notice retires it, and must not
 * take it for a first touch of generation g+1. */

#define RING_ROTATE 1
#define RING_NAME "label_reuse_ring_db_rotate"
#include "label_reuse_ring_db.c"
