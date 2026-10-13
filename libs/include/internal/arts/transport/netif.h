/******************************************************************************
 * Copyright 2019 Battelle Memorial Institute
 * Licensed under the Apache License, Version 2.0
 ******************************************************************************/
#ifndef ARTS_TRANSPORT_NETIF_H
#define ARTS_TRANSPORT_NETIF_H
#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>

#include "arts/system/print.h"

/* The one rule by which `net_interface` names an interface, for every plane
 * that honours it: the first AF_INET address of the interface with exactly
 * that name, else of the first interface whose name starts with it, so one
 * config can say "ib" across hosts whose suffixes differ.  `ifname` is never
 * empty (an empty value is unset, and every name starts with it).  Returns
 * false when no interface matches or the matching ones carry no IPv4 address;
 * failing to list the interfaces at all is fatal. */
static inline bool arts_netif_ipv4_addr(const char *ifname,
                                        struct sockaddr_in *out) {
  assert(ifname != NULL && ifname[0] != '\0');
  struct ifaddrs *ifap = NULL;
  if (getifaddrs(&ifap) != 0) {
    ARTS_ERROR("getifaddrs: %s", strerror(errno));
  }
  bool found = false;
  size_t want_len = strlen(ifname);
  for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    bool is_exact = strcmp(ifa->ifa_name, ifname) == 0;
    bool is_prefix = strncmp(ifa->ifa_name, ifname, want_len) == 0;
    if (!is_exact && !is_prefix) {
      continue;
    }
    if (is_exact || !found) {
      memcpy(out, ifa->ifa_addr, sizeof(*out));
      found = true;
    }
    if (is_exact) {
      break;
    }
  }
  freeifaddrs(ifap);
  return found;
}

#ifdef __cplusplus
}
#endif
#endif /* ARTS_TRANSPORT_NETIF_H */
