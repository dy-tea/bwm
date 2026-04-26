#include "server.h"
#include "config.h"
#include "log.h"
#include <wlr/util/log.h>
#include <stdio.h>
#include <string.h>

struct bwm_server server = {0};

static void usage(const char *argv0) {
  printf("Usage: %s [-c <config-dir>]\n", argv0);
  printf("  -c, --config <dir>  Use specified config directory instead of default\n");
}

int main(int argc, char *argv[]) {
  const char *config_dir = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
        usage(argv[0]);
        return 1;
      }
      config_dir = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  if (log_init(NULL) != 0) {
    fprintf(stderr, "Error: Failed to initialize logging\n");
    return 1;
  }
  
  if (log_setup_signals() != 0) {
    fprintf(stderr, "Error: Failed to setup signal handlers\n");
    return 1;
  }
  
  config_init_with_config_dir(config_dir);
  server_init();
  int ret = server_run();
  server_fini();
  log_fini();
  return ret;
}
