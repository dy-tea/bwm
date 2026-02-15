#include "server.h"
#include <wlr/util/log.h>

struct bwm_server server;

int main(void) {
  wlr_log_init(WLR_DEBUG, NULL);
  server = init();
  return run(&server);
}
