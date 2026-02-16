#include "server.h"
#include <wlr/util/log.h>

struct bwm_server server = {0};

int main(void) {
  wlr_log_init(WLR_DEBUG, NULL);
  server_init();
  int ret = server_run();
  server_fini();
  return ret;
}
