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
#include <fcntl.h>
#include <stdbool.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
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
bool arts_transport_runtime_uses_rdma(void);
#define RRECV(fd, buf, len, flags)                                             \
  (arts_transport_runtime_uses_rdma() ? rrecv((fd), (buf), (len), (flags))     \
                                      : recv((fd), (buf), (len), (flags)))
#define RSEND(fd, buf, len, flags)                                             \
  (arts_transport_runtime_uses_rdma() ? rsend((fd), (buf), (len), (flags))     \
                                      : send((fd), (buf), (len), (flags)))
#define RLISTEN(fd, backlog)                                                   \
  (arts_transport_runtime_uses_rdma() ? rlisten((fd), (backlog))               \
                                      : listen((fd), (backlog)))
#define RPOLL(fds, nfds, timeout)                                              \
  (arts_transport_runtime_uses_rdma() ? rpoll((fds), (nfds), (timeout))        \
                                      : poll((fds), (nfds), (timeout)))
#define RSELECT(nfds, readfds, writefds, exceptfds, timeout)                   \
  (arts_transport_runtime_uses_rdma()                                          \
       ? rselect((nfds), (readfds), (writefds), (exceptfds), (timeout))        \
       : select((nfds), (readfds), (writefds), (exceptfds), (timeout)))
#define RBIND(fd, addr, len)                                                   \
  (arts_transport_runtime_uses_rdma() ? rbind((fd), (addr), (len))             \
                                      : bind((fd), (addr), (len)))
#define RCLOSE(fd)                                                             \
  (arts_transport_runtime_uses_rdma() ? rclose((fd)) : close((fd)))
#define RACCEPT(fd, addr, len)                                                 \
  (arts_transport_runtime_uses_rdma() ? raccept((fd), (addr), (len))           \
                                      : accept((fd), (addr), (len)))
#define RCONNECT(fd, addr, len)                                                \
  (arts_transport_runtime_uses_rdma() ? rconnect((fd), (addr), (len))          \
                                      : connect((fd), (addr), (len)))
#define RGETSOCKOPT(fd, level, optname, optval, optlen)                        \
  (arts_transport_runtime_uses_rdma()                                          \
       ? rgetsockopt((fd), (level), (optname), (optval), (optlen))             \
       : getsockopt((fd), (level), (optname), (optval), (optlen)))
#define RSETSOCKOPT(fd, level, optname, optval, optlen)                        \
  (arts_transport_runtime_uses_rdma()                                          \
       ? rsetsockopt((fd), (level), (optname), (optval), (optlen))             \
       : setsockopt((fd), (level), (optname), (optval), (optlen)))
#define RSOCKET(domain, type, protocol)                                        \
  (arts_transport_runtime_uses_rdma() ? rsocket((domain), (type), (protocol))  \
                                      : socket((domain), (type), (protocol)))
#define RSHUTDOWN(fd, how)                                                     \
  (arts_transport_runtime_uses_rdma() ? rshutdown((fd), (how))                 \
                                      : shutdown((fd), (how)))
#define RF_GETFL(fd)                                                           \
  (arts_transport_runtime_uses_rdma() ? rfcntl((fd), F_GETFL)                 \
                                      : fcntl((fd), F_GETFL))
#define RF_SETFL(fd, flags)                                                    \
  (arts_transport_runtime_uses_rdma() ? rfcntl((fd), F_SETFL, (flags))         \
                                      : fcntl((fd), F_SETFL, (flags)))
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
