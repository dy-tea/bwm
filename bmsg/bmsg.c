#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#define BWM_SOCKET_ENV "BWM_SOCKET"
#define BWM_SOCKET_PATH_TEMPLATE "/tmp/bwm-%d.sock"
#define BWM_BUFSIZ 4096

static void err(const char *msg) {
  fprintf(stderr, "%s", msg);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
  int sock_fd;
  struct sockaddr_un sock_address;
  char msg[BWM_BUFSIZ], rsp[BWM_BUFSIZ];

  if (argc < 2)
    err("No arguments given.\n");

  sock_address.sun_family = AF_UNIX;

  char *sp = getenv(BWM_SOCKET_ENV);
  if (sp != NULL)
    snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s", sp);
  else
    snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), BWM_SOCKET_PATH_TEMPLATE, getuid());


  if (strcmp(argv[1], "--print-socket-path") == 0) {
    printf("%s\n", sock_address.sun_path);
    return EXIT_SUCCESS;
  }

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd == -1)
    err("Failed to create the socket.\n");

  if (connect(sock_fd, (struct sockaddr *)&sock_address, sizeof(sock_address)) == -1) {
    close(sock_fd);
    err("Failed to connect to the socket.\n");
  }

  argc--;
  argv++;
  int msg_len = 0;

  for (int offset = 0, rem = sizeof(msg), n = 0; argc > 0 && rem > 0; offset += n, rem -= n, argc--, argv++) {
    n = snprintf(msg + offset, rem, "%s%c", *argv, 0);
    msg_len += n;
  }

  if (send(sock_fd, msg, msg_len, 0) == -1) {
    close(sock_fd);
    err("Failed to send the data.\n");
  }

  int ret = EXIT_SUCCESS, nb;

  struct pollfd fds[] = {
    {sock_fd, POLLIN | POLLHUP, 0},
  };

  while (poll(fds, 1, -1) > 0) {
    if (fds[0].revents & (POLLHUP | POLLIN)) {
      if ((nb = recv(sock_fd, rsp, sizeof(rsp) - 1, 0)) > 0) {
        rsp[nb] = '\0';
        if (rsp[0] == '\x01') {
          ret = EXIT_FAILURE;
          fprintf(stderr, "%s", rsp + 1);
          fflush(stderr);
        } else {
          fprintf(stdout, "%s", rsp + 1);
          fflush(stdout);
        }
      }
      break;
    }
  }

  close(sock_fd);
  return ret;
}
