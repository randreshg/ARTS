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
#ifndef ARTS_TRANSPORT_CONNECTION_H
#define ARTS_TRANSPORT_CONNECTION_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ARTS_USE_RDMA
#if defined(__has_include)
#if __has_include(<rdma/rsocket.h>)
#include <rdma/rsocket.h>
#elif __has_include(<rdma/RSOCKET.h>)
#include <rdma/RSOCKET.h>
#else
#error "ARTS_USE_RDMA requires rdma/rsocket.h or rdma/RSOCKET.h"
#endif
#else
#include <rdma/rsocket.h>
#endif
#define RRECV rrecv
#define RSEND rsend
#define RLISTEN rlisten
#define RPOLL rpoll
#define RSELECT rselect
#define RBIND rbind
#define RCLOSE rclose
#define RACCEPT raccept
#define RCONNECT rconnect
#define RGETSOCKOPT rgetsockopt
#define RSETSOCKOPT rsetsockopt
#define RSOCKET rsocket
#define RSHUTDOWN rshutdown
#define RF_GETFL(fd) rfcntl((fd), F_GETFL)
#define RF_SETFL(fd, flags) rfcntl((fd), F_SETFL, (flags))
#else
#include <sys/poll.h>
#define RRECV recv
#define RSEND send
#define RLISTEN listen
#define RPOLL poll
#define RSELECT select
#define RBIND bind
#define RCLOSE close
#define RACCEPT accept
#define RCONNECT connect
#define RGETSOCKOPT getsockopt
#define RSETSOCKOPT setsockopt
#define RSOCKET socket
#define RSHUTDOWN shutdown
#define RF_GETFL(fd) fcntl((fd), F_GETFL)
#define RF_SETFL(fd, flags) fcntl((fd), F_SETFL, (flags))
#endif

#ifdef __cplusplus
}
#endif

#endif
