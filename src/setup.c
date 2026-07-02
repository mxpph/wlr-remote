#include "setup.h"

#include "common.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

volatile sig_atomic_t quit = 0;

static void handle_signal(int signum) { quit = 1; }

void setup_signals(void) {
  struct sigaction sa = {0};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = &handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

size_t setup_password(char *psk_key) {
  struct termios term;
  tcgetattr(STDIN_FILENO, &term);
  term.c_lflag &= ~ECHO;

  printf("Create a password (max %d characters): ", DTLS_PSK_MAX_KEY_LEN);
  fflush(stdout);

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term) < 0) {
    fprintf(stderr,
            "warn: Couldn't disable ECHO, password will be printed to screen");
  };
  char *res = fgets(psk_key, PSK_BUFFER_SIZE, stdin);
  term.c_lflag |= ECHO;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
  putchar('\n'); // ECHO was off, so we need to print a newline ourselves

  if (!res) {
    fprintf(stderr, "error: Failed to read password from stdin\n");
    exit(EXIT_FAILURE);
  }

  const size_t len = strcspn(psk_key, "\n");
  if (len == PSK_BUFFER_SIZE - 1) {
    fprintf(stderr, "error: Password too long\n");
    exit(EXIT_FAILURE);
  }
  if (len == 0) {
    fprintf(stderr, "error: Password cannot be empty\n");
    exit(EXIT_FAILURE);
  }
  psk_key[len] = '\0';
  return len;
}

unsigned short setup_parse_port(const int argc, const char **argv) {
  if (argc <= 1) {
    return DEFAULT_PORT;
  }
  const long input = strtol(argv[1], NULL, 10);
  if (0 < input && input <= UINT16_MAX) {
    return (unsigned short)input;
  }
  fprintf(stderr, "warn: Invalid port '%s', using default %u\n", argv[1],
          DEFAULT_PORT);
  return DEFAULT_PORT;
}
