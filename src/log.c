#include "log.h"
#include <wlr/util/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_LOG_LINES 10000
#define MAX_LOG_FILES 5
#define LOG_FILENAME_BASE "bwm"

static FILE *log_file = NULL;
static char log_path[256] = {0};
static char log_dir[256] = {0};
static unsigned int current_line_count = 0;
static unsigned int rotation_count = 0;

static void rotate_log_file(void);

static void log_callback(enum wlr_log_importance importance, const char *fmt, va_list args) {
  // check if rotation is needed
  if (log_file && current_line_count >= MAX_LOG_LINES)
    rotate_log_file();

  // get current time
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

  // map importance level to string
  const char *level_str = "UNKN";
  switch (importance) {
    case WLR_SILENT: level_str = "SILE"; break;
    case WLR_ERROR: level_str = "ERR "; break;
    case WLR_INFO: level_str = "INFO"; break;
    case WLR_DEBUG: level_str = "DBG "; break;
    default: level_str = "UNKN"; break;
  }

  // print to stdout
  fprintf(stdout, "[%s] %s ", time_str, level_str);
  vfprintf(stdout, fmt, args);
  fprintf(stdout, "\n");
  fflush(stdout);

  // print to file
  if (log_file) {
    fprintf(log_file, "[%s] %s ", time_str, level_str);
    vfprintf(log_file, fmt, args);
    fprintf(log_file, "\n");
    fflush(log_file);
    current_line_count++;
  }
}

static void rotate_log_file(void) {
  if (!log_file)
    return;

  // close current file
  fprintf(log_file, "########## Log rotation ##########\n");
  fflush(log_file);
  fclose(log_file);
  log_file = NULL;

  char rotated_path[256];
  snprintf(rotated_path, sizeof(rotated_path), "%s/%s.%u.log",
  	log_dir, LOG_FILENAME_BASE, rotation_count++ % MAX_LOG_FILES);

  // rename current log to rotated version
  if (rename(log_path, rotated_path) != 0) {
    fprintf(stderr, "ERROR: Failed to rotate log file: %s\n", strerror(errno));
    log_file = fopen(log_path, "a");
    return;
  }

  fprintf(stderr, "Rotated log to: %s\n", rotated_path);

  // open new log file
  log_file = fopen(log_path, "a");
  if (!log_file) {
    fprintf(stderr, "ERROR: Failed to open new log file: %s\n", log_path);
    return;
  }

  current_line_count = 0;

  // log rotation marker
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
  fprintf(log_file, "########## bwm startup (%s) ##########\n", time_str);
  fflush(log_file);
  current_line_count++;
}

static void signal_handler(int sig) {
  const char *sig_name = "UNKNOWN";
  switch (sig) {
    case SIGSEGV: sig_name = "SIGSEGV (Segmentation Fault)"; break;
    case SIGBUS: sig_name = "SIGBUS (Bus Error)"; break;
    case SIGABRT: sig_name = "SIGABRT (Abort)"; break;
    case SIGFPE: sig_name = "SIGFPE (Floating Point Error)"; break;
    case SIGILL: sig_name = "SIGILL (Illegal Instruction)"; break;
  }

  // get backtrace
  void *addrlist[32];
  int addrlen = backtrace(addrlist, 32);

  // log to stdout
  write(STDOUT_FILENO, "\n########## CRASH REPORT ##########\n", 36);
  write(STDOUT_FILENO, "Signal: ", 8);
  write(STDOUT_FILENO, sig_name, strlen(sig_name));
  write(STDOUT_FILENO, "\nBacktrace:\n", 12);
  backtrace_symbols_fd(addrlist, addrlen, STDOUT_FILENO);
  write(STDOUT_FILENO, "##################################\n\n", 35);

  // always log crashes
  if (log_file) {
    fprintf(log_file, "\n########## CRASH REPORT ##########\n");
    fprintf(log_file, "Signal: %s (%d)\n", sig_name, sig);
    fprintf(log_file, "Backtrace:\n");
    backtrace_symbols_fd(addrlist, addrlen, fileno(log_file));
    fprintf(log_file, "##################################\n\n");
    fflush(log_file);
  }

  // exit with error code
  _exit(128 + sig);
}

int log_init(const char *log_file_path) {
  // determine log file path and directory
  if (log_file_path) {
    snprintf(log_path, sizeof(log_path), "%s", log_file_path);

    // extract dir from path
    strcpy(log_dir, log_file_path);
    char *last_slash = strrchr(log_dir, '/');
    if (last_slash) *last_slash = '\0';
    else strcpy(log_dir, ".");
  } else {
    const char *home = getenv("HOME");
    if (!home) {
      fprintf(stderr, "ERROR: HOME environment variable not set\n");
      return -1;
    }

    snprintf(log_dir, sizeof(log_dir), "%s/.cache/bwm", home);

    // try to create directories
    struct stat st = {0};
    if (stat(log_dir, &st) == -1) {
      char mkdir_cmd[512];
      snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", log_dir);
      if (system(mkdir_cmd) != 0) {
        fprintf(stderr, "ERROR: Failed to create log directory: %s\n", log_dir);
        return -1;
      }
    }

    snprintf(log_path, sizeof(log_path), "%s/bwm.log", log_dir);
  }

  // open log file for appending
  log_file = fopen(log_path, "a");
  if (!log_file) {
    fprintf(stderr, "ERROR: Failed to open log file: %s (%s)\n", log_path, strerror(errno));
    return -1;
  }

  // count existing lines in log file
  current_line_count = 0;
  FILE *temp_file = fopen(log_path, "r");
  if (temp_file) {
    int c;
    while ((c = fgetc(temp_file)) != EOF)
      if (c == '\n')
        current_line_count++;
    fclose(temp_file);
  }

  fprintf(stdout, "Logging to: %s\n", log_path);
  fprintf(stdout, "Log rotation: %u lines per file, keeping %u files (0-%u)\n",
    MAX_LOG_LINES, MAX_LOG_FILES, MAX_LOG_FILES - 1);

  // log startup
  fprintf(log_file, "\n");
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
  fprintf(log_file, "########## bwm startup (%s) ##########\n", time_str);
  fflush(log_file);
  current_line_count += 2;

  wlr_log_init(WLR_DEBUG, log_callback);

  return 0;
}

int log_setup_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;

  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);

  return 0;
}

void log_fini(void) {
  if (log_file) {
    fprintf(log_file, "########## bwm shutdown ##########\n\n");
    fflush(log_file);
    fclose(log_file);
    log_file = NULL;
  }
}

const char *log_get_path(void) {
  return log_path[0] != '\0' ? log_path : NULL;
}
