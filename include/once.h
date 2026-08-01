#include <wlr/util/log.h>

#if DEBUG
#define ONCE() \
  do { \
    static int _called_once_flag_##__LINE__ = 0; \
    if (_called_once_flag_##__LINE__) \
     	wlr_log(WLR_ERROR, "Function %s called more than once", __func__); \
    _called_once_flag_##__LINE__ = 1; \
  } while(0)
#else
#define ONCE() ;
#endif
