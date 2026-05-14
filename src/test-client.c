#include <arpa/inet.h>
#include <dtls.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 39076
#define SERVER_IP "127.0.0.1"

#define DELAY 10000 // microseconds
#define ITERATIONS 50

static const unsigned char PSK_ID[] = "user";
static const unsigned char PSK_KEY[] = "pass";

struct mouse_packet {
  int32_t dx;
  int32_t dy;
  uint16_t button;
  uint16_t button_state;
};

static int is_connected = 0;

static int net_handle_send(dtls_context_t *ctx, session_t *session,
                           unsigned char *buf, size_t len) {
  const int fd = *(int *)dtls_get_app_data(ctx);
  return sendto(fd, buf, len, 0, &session->addr.sa, session->size);
}

static int net_handle_receive(dtls_context_t *ctx, session_t *session,
                              unsigned char *data, size_t len) {
  return 0;
}

static int net_handle_event(dtls_context_t *ctx, session_t *session,
                            dtls_alert_level_t level, unsigned short code) {
  if (level == 0 && code == DTLS_EVENT_CONNECTED) {
    is_connected = 1;
  }
  return 0;
}

static int net_get_psk_info(dtls_context_t *ctx, const session_t *session,
                            dtls_credentials_type_t type,
                            const unsigned char *id, size_t id_len,
                            unsigned char *result, size_t result_length) {
  if (type == DTLS_PSK_IDENTITY) {
    if (result_length < sizeof(PSK_ID) - 1) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }
    memcpy(result, PSK_ID, sizeof(PSK_ID) - 1);
    return sizeof(PSK_ID) - 1;
  }

  if (type == DTLS_PSK_KEY) {
    if (result_length < sizeof(PSK_KEY) - 1) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }
    memcpy(result, PSK_KEY, sizeof(PSK_KEY) - 1);
    return sizeof(PSK_KEY) - 1;
  }

  return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
}

static dtls_handler_t dtls_callbacks = {.write = net_handle_send,
                                        .read = net_handle_receive,
                                        .event = net_handle_event,
                                        .get_psk_info = net_get_psk_info};

int main(void) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  struct sockaddr_in srv_addr = {0};
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(SERVER_PORT);
  if (inet_pton(AF_INET, SERVER_IP, &srv_addr.sin_addr) <= 0) {
    perror("inet_pton");
    close(fd);
    return EXIT_FAILURE;
  }

  session_t session = {0};
  session.size = sizeof(srv_addr);
  memcpy(&session.addr.sa, &srv_addr, sizeof(srv_addr));

  dtls_init();
  dtls_context_t *ctx = dtls_new_context((void *)&fd);
  if (!ctx) {
    fprintf(stderr, "error: Failed to create DTLS context\n");
    close(fd);
    return EXIT_FAILURE;
  }
  dtls_set_handler(ctx, &dtls_callbacks);

  if (dtls_connect(ctx, &session) < 0) {
    fprintf(stderr, "error: dtls_connect failed\n");
    goto cleanup;
  }

  printf("Handshaking with %s:%d...\n", SERVER_IP, SERVER_PORT);

  while (!is_connected) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    const int ready = select(fd + 1, &rfds, NULL, NULL, &tv);

    if (ready < 0) {
      perror("select");
      break;
    }
    if (ready == 0) {
      fprintf(stderr, "error: Handshake timeout\n");
      break;
    }

    if (FD_ISSET(fd, &rfds)) {
      unsigned char buf[DTLS_MAX_BUF];
      session_t recv_session = {0};
      recv_session.size = sizeof(recv_session.addr);
      const ssize_t len = recvfrom(fd, buf, sizeof(buf), 0,
                                   &recv_session.addr.sa, &recv_session.size);
      if (len > 0) {
        dtls_handle_message(ctx, &recv_session, buf, (int)len);
      }
    }
  }

  if (is_connected) {
    struct mouse_packet pkt;
    pkt.dx = (int32_t)htonl((uint32_t)10);
    pkt.dy = (int32_t)htonl((uint32_t)10);
    pkt.button = htons(0);
    pkt.button_state = htons(0);

    for (int i = 0; i < ITERATIONS; ++i) {
      const int res =
          dtls_write(ctx, &session, (unsigned char *)&pkt, sizeof(pkt));
      if (res > 0) {
        printf("Success: Sent mouse movement packet %d (%d bytes).\n", i, res);
      } else {
        fprintf(stderr, "error: dtls_write failed with code %d\n", res);
      }
      usleep((i < ITERATIONS - 1) * DELAY);
    }
  }
cleanup:
  dtls_free_context(ctx);
  close(fd);
  return is_connected ? EXIT_SUCCESS : EXIT_FAILURE;
}
