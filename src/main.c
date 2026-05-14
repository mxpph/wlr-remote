#include <dtls.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-protocol.h"

#ifndef WL_POINTER_DEFAULT_PORT
#define WL_POINTER_DEFAULT_PORT 39076
#endif

#define DGRAM_BUF_SIZE 1500

static const unsigned char PSK_ID[] = "user";
static const unsigned char PSK_KEY[] = "pass"; // TODO user input

static volatile bool quit = false;

struct client_state {
  struct wl_seat *seat;
  struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
  struct zwlr_virtual_pointer_v1 *virtual_pointer;
  int sock_fd;
};

struct mouse_packet {
  int32_t dx;
  int32_t dy;
  uint16_t button;
  uint16_t button_state;
};

static void handle_signal(int signum) { quit = true; }

static void setup_signals(void) {
  struct sigaction sa = {0};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = &handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static int net_setup_udp_socket(const unsigned short port) {
  const int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(sock);
    return -1;
  }
  return sock;
}

static int net_setup_epoll(const int wl_fd, const int sock_fd) {
  const int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    return -1;
  }
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = wl_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev) < 0) {
    goto err_close_fds;
  }
  ev.events = EPOLLIN;
  ev.data.fd = sock_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev) < 0) {
    goto err_close_fds;
  }
  return epoll_fd;
err_close_fds:
  perror("epoll_ctl");
  close(sock_fd);
  close(epoll_fd);
  return -1;
}

static void handle_packet(const struct client_state *state,
                          const struct mouse_packet *packet) {
  if (packet->button_state > 1) {
    fprintf(stderr, "warn: Received packet with invalid button state %hu\n",
            packet->button_state);
    return;
  }
  switch (packet->button) {
  case BTN_LEFT:
  case BTN_RIGHT:
  case BTN_MIDDLE:
  case BTN_SIDE:
  case BTN_EXTRA:
    zwlr_virtual_pointer_v1_button(state->virtual_pointer, 0, packet->button,
                                   packet->button_state);
    break;
  case 0:
    break;
  default:
    fprintf(stderr, "warn: Received packet with unknown button %hu\n",
            packet->button);
    return;
  }
  const wl_fixed_t dx = wl_fixed_from_int(packet->dx);
  const wl_fixed_t dy = wl_fixed_from_int(packet->dy);
  zwlr_virtual_pointer_v1_motion(state->virtual_pointer, 0, dx, dy);
  zwlr_virtual_pointer_v1_frame(state->virtual_pointer);
}

static int net_handle_epoll(dtls_context_t *ctx, const int sock_fd) {
  static unsigned char buffer[DTLS_MAX_BUF];
  session_t session;
  while (true) {
    memset(&session, 0, sizeof(session));
    session.size = sizeof(session.addr);
    const ssize_t bytes_read =
        recvfrom(sock_fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_TRUNC,
                 &session.addr.sa, &session.size);
    if (bytes_read < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0; // handled everything
      }
      perror("recvfrom");
      return -1;
    }
    if (bytes_read > DTLS_MAX_BUF) {
      fprintf(stderr, "warn: Packet of size %zd exceeds buffer\n", bytes_read);
    }
    const int res = dtls_handle_message(ctx, &session, buffer, (int)bytes_read);
    if (res < 0) {
      return res;
    }
  }
}

static int net_handle_send(dtls_context_t *ctx, session_t *session,
                           unsigned char *buf, size_t len) {
  const struct client_state *state = dtls_get_app_data(ctx);
  const ssize_t sent = sendto(state->sock_fd, buf, len, MSG_DONTWAIT,
                              &session->addr.sa, session->size);
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
      return (int)len; // pretend we succeeded, retransmission should occur
    }
    perror("sendto");
    return -1;
  }
  return (int)sent;
}

static int net_handle_receive(dtls_context_t *ctx, session_t *session,
                              unsigned char *data, size_t len) {
  if (len == sizeof(struct mouse_packet)) {
    struct mouse_packet packet;
    memcpy(&packet, data, sizeof(packet));
    packet.dx = (int32_t)ntohl((uint32_t)packet.dx);
    packet.dy = (int32_t)ntohl((uint32_t)packet.dy);
    packet.button = ntohs(packet.button);
    packet.button_state = ntohs(packet.button_state);
    const struct client_state *state =
        (struct client_state *)dtls_get_app_data(ctx);
    handle_packet(state, &packet);
  } else {
    fprintf(stderr, "warn: Received packet of unexpected size %zu\n", len);
  }
  return 0;
}

static int net_handle_event(dtls_context_t *ctx, session_t *session,
                            dtls_alert_level_t level, unsigned short code) {
  char client_addr[INET_ADDRSTRLEN];
  if (!inet_ntop(session->addr.sa.sa_family, &session->addr.sin.sin_addr,
                 client_addr, sizeof(client_addr))) {
    fprintf(stderr, "warn: Failed to convert client address to string\n");
    perror("inet_ntop");
  }
  if (level > 0) {
    if (code == DTLS_ALERT_CLOSE_NOTIFY) {
      printf("Connection closed by client %s\n", client_addr);
    } else {
      fprintf(stderr,
              "warn: Received alert with level %u and code %u from client %s\n",
              level, code, client_addr);
    }
    return 0;
  }
  switch (code) {
  case DTLS_EVENT_CONNECT:
    printf("Handshake initiated with client %s\n", client_addr);
    break;
  case DTLS_EVENT_CONNECTED:
    printf("Handshake completed with client %s\n", client_addr);
    break;
  default:
    break;
  }
  return 0;
}

static int net_get_psk_info(dtls_context_t *ctx, const session_t *session,
                            dtls_credentials_type_t type,
                            const unsigned char *id, size_t id_len,
                            unsigned char *result, size_t result_length) {
  if (type != DTLS_PSK_KEY) {
    return 0;
  }

  if (id_len == sizeof(PSK_ID) - 1 && memcmp(id, PSK_ID, id_len) == 0) {
    if (result_length < sizeof(PSK_KEY) - 1) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }
    memcpy(result, PSK_KEY, sizeof(PSK_KEY) - 1);
    return sizeof(PSK_KEY) - 1;
  }
  return dtls_alert_fatal_create(DTLS_ALERT_DECRYPT_ERROR);
}

static dtls_handler_t dtls_callbacks = {.write = &net_handle_send,
                                        .read = &net_handle_receive,
                                        .event = &net_handle_event,
                                        .get_psk_info = &net_get_psk_info};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  struct client_state *state = data;

  if (!strcmp(interface, "wl_seat")) {
    state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
  } else if (!strcmp(interface, "zwlr_virtual_pointer_manager_v1")) {
    state->pointer_manager = wl_registry_bind(
        registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
  }
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name) {
  // Empty
}

static const struct wl_registry_listener registry_listener = {
    .global = &registry_handle_global,
    .global_remove = &registry_handle_global_remove,
};

int main(const int argc, const char **argv) {
  bool failure = false;
  unsigned short port = WL_POINTER_DEFAULT_PORT;
  if (argc > 1) {
    const long input = strtol(argv[1], NULL, 10);
    if (0 < input && input < UINT16_MAX) {
      port = (unsigned short)input;
    } else {
      fprintf(stderr, "warn: Invalid port '%s', using default %u\n", argv[1],
              WL_POINTER_DEFAULT_PORT);
    }
  }
  setup_signals();

  struct client_state state = {0};
  struct wl_display *display = wl_display_connect(NULL);
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, &state);
  wl_display_roundtrip(display);
  if (!state.seat || !state.pointer_manager) {
    fprintf(stderr,
            "error: Compositor doesn't support the required protocol\n");
    failure = true;
    goto err_free_registry;
  }
  state.virtual_pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
          state.pointer_manager, state.seat);

  int wl_fd, sock_fd, epoll_fd;
  if ((wl_fd = wl_display_get_fd(display)) < 0 ||
      (sock_fd = net_setup_udp_socket(port)) < 0 ||
      (epoll_fd = net_setup_epoll(wl_fd, sock_fd)) < 0) {
    failure = true;
    goto err_free_virtual_pointer;
  }
  state.sock_fd = sock_fd;

  dtls_init();
  dtls_context_t *dtls_context = dtls_new_context(&state);
  dtls_set_handler(dtls_context, &dtls_callbacks);

  printf("Listening for mouse movements on UDP port %hu...\n", port);
  struct epoll_event events[2];
  while (!quit) {
    wl_display_dispatch_pending(display);
    wl_display_flush(display);
    const int ready = epoll_wait(epoll_fd, events, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("epoll_wait");
      goto err_free_all;
    }
    for (int i = 0; i < ready; ++i) {
      const int event_fd = events[i].data.fd;
      if ((event_fd == wl_fd && wl_display_dispatch(display) < 0) ||
          (event_fd == sock_fd &&
           net_handle_epoll(dtls_context, sock_fd) < 0)) {
        goto err_free_all;
      }
    }
  }
  // Cleanup
err_free_all:
  close(sock_fd);
  close(epoll_fd);
  dtls_free_context(dtls_context);
err_free_virtual_pointer:
  zwlr_virtual_pointer_v1_destroy(state.virtual_pointer);
  zwlr_virtual_pointer_manager_v1_destroy(state.pointer_manager);
  wl_seat_destroy(state.seat);
err_free_registry:
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  printf("Shutting down.\n");
  return failure ? EXIT_FAILURE : EXIT_SUCCESS;
}
