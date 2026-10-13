/* SPDX-License-Identifier: Apache-2.0
 *
 * `provider`, `regpool_slab_mb` and `net_interface` cfg-key parsing
 * (config.c).
 *
 * Property under test
 * -------------------
 *   provider         - CONFIG_STRING, passed through to fi_getinfo's
 *                       prov_name hint by arts_net_init(); unset means
 *                       auto-select (NULL field).
 *   regpool_slab_mb   - CONFIG_UINT, the registered-memory pool's base slab
 *                       size; default 256.
 *   net_interface    - an empty value, from the cfg or from the environment,
 *                       is unset (NULL field), so the bootstrap and the fabric
 *                       agree that no interface was named; as a name prefix
 *                       it would match every interface.
 *
 * This is a config-parser test: it calls arts_config_load() directly against
 * crafted temp cfgs (no runtime started, no fabric brought up, no ports
 * bound).  Self-contained: it sets its own ARTS_CONFIG and clears the
 * inherited `ports`/`port_count`/`provider`/`regpool_slab_mb`/`net_interface`
 * env overrides, since an environment variable named after a key overrides the
 * cfg.  Orthogonal to the coherence protocol axis (config.c is
 * protocol-agnostic).
 */

#include "arts.h"
#include "arts/system/config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_cfg(char *path_out, size_t path_cap, const char *tag,
                     const char *extra_lines) {
  snprintf(path_out, path_cap, "config_provider_regpool_parse_%s_%ld.cfg",
           tag, (long)getpid());
  FILE *f = fopen(path_out, "w");
  if (!f) {
    return -1;
  }
  fputs("[ARTS]\n"
        "launcher=local\n"
        "node_count=1\n"
        "worker_threads=2\n"
        "route_table_size=14\n",
        f);
  fputs(extra_lines, f);
  (void)fclose(f);
  return 0;
}

static int load_cfg(const char *tag, const char *extra_lines,
                    const char *netif_env, struct arts_config_s *out) {
  char cfg_path[256];
  if (write_cfg(cfg_path, sizeof(cfg_path), tag, extra_lines) != 0) {
    return -1;
  }
  setenv("ARTS_CONFIG", cfg_path, 1);
  unsetenv("ports");
  unsetenv("port_count");
  unsetenv("provider");
  unsetenv("regpool_slab_mb");
  if (netif_env != NULL) {
    setenv("net_interface", netif_env, 1);
  } else {
    unsetenv("net_interface");
  }
  arts_config_load(out);
  unsetenv("net_interface");
  (void)remove(cfg_path);
  return 0;
}

int main(void) {
  int fails = 0;

  /* (A) Explicit provider + regpool_slab_mb parse into the struct verbatim. */
  {
    struct arts_config_s config;
    if (load_cfg("explicit", "provider=verbs\nregpool_slab_mb=128\n", NULL,
                 &config) != 0) {
      printf("FAIL config_provider_regpool_parse: cannot write explicit "
             "cfg\n");
      return 1;
    }
    if (!config.provider || strcmp(config.provider, "verbs") != 0) {
      printf("FAIL config_provider_regpool_parse: provider=%s expected "
             "\"verbs\"\n",
             config.provider ? config.provider : "(null)");
      fails++;
    }
    if (config.regpool_slab_mb != 128) {
      printf("FAIL config_provider_regpool_parse: regpool_slab_mb=%u "
             "expected 128\n",
             config.regpool_slab_mb);
      fails++;
    }
    arts_config_destroy(&config);
  }

  /* (B) Unconfigured: provider stays NULL (auto) and regpool_slab_mb takes
   * the table's built-in default — the base slab the pool carves per NUMA
   * node when a cfg names no size. */
  {
    struct arts_config_s config;
    if (load_cfg("default", "", NULL, &config) != 0) {
      printf(
          "FAIL config_provider_regpool_parse: cannot write default cfg\n");
      return 1;
    }
    if (config.provider != NULL) {
      printf("FAIL config_provider_regpool_parse: provider=%s expected "
             "unset (auto)\n",
             config.provider);
      fails++;
    }
    if (config.regpool_slab_mb != 256) {
      printf("FAIL config_provider_regpool_parse: regpool_slab_mb=%u "
             "expected default 256\n",
             config.regpool_slab_mb);
      fails++;
    }
    arts_config_destroy(&config);
  }

  /* (C) net_interface: a name is kept verbatim; an empty value is unset
   * whether the cfg or the environment supplies it. */
  {
    static const struct {
      const char *tag;
      const char *cfg;
      const char *env;
      const char *expect;
    } cases[] = {
        {"netif_named", "net_interface=ib0\n", NULL, "ib0"},
        {"netif_cfg_empty", "net_interface=\n", NULL, NULL},
        {"netif_env_empty", "net_interface=ib0\n", "", NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      struct arts_config_s config;
      if (load_cfg(cases[i].tag, cases[i].cfg, cases[i].env, &config) != 0) {
        printf("FAIL config_provider_regpool_parse: cannot write %s cfg\n",
               cases[i].tag);
        return 1;
      }
      bool ok = cases[i].expect == NULL
                    ? config.net_interface == NULL
                    : (config.net_interface != NULL &&
                       strcmp(config.net_interface, cases[i].expect) == 0);
      if (!ok) {
        printf("FAIL config_provider_regpool_parse: %s: net_interface=%s "
               "expected %s\n",
               cases[i].tag,
               config.net_interface ? config.net_interface : "(null)",
               cases[i].expect ? cases[i].expect : "(null)");
        fails++;
      }
      arts_config_destroy(&config);
    }
  }

  if (fails == 0) {
    printf("PASS config_provider_regpool_parse: provider + regpool_slab_mb "
           "parse explicit values and the unconfigured default (NULL, 256); "
           "an empty net_interface is unset\n");
    return 0;
  }
  return 1;
}
