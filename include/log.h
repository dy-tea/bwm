#pragma once

#include <stdarg.h>

int log_init(const char *log_file);
int log_setup_signals(void);
void log_fini(void);
const char *log_get_path(void);
