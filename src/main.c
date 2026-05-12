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

static volatile int quit = 0;

struct client_state {
  struct wl_seat *seat;
  struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
  struct zwlr_virtual_pointer_v1 *virtual_pointer;
};

struct mouse_packet {
  int32_t dx;
  int32_t dy;
  uint16_t button;
  uint16_t button_state;
};

static void handle_signal(int signum) { quit = 1; }

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

static int net_setup_epoll(const int wl_fd, const int net_fd) {
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
  ev.data.fd = net_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, net_fd, &ev) < 0) {
    goto err_close_fds;
  }
  return epoll_fd;
err_close_fds:
  perror("epoll_ctl");
  close(net_fd);
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

static bool net_receive_packet(const struct client_state *state,
                               const int net_fd) {
  struct mouse_packet packet;
  const ssize_t bytes_read = recv(net_fd, &packet, sizeof(packet), 0);
  if (bytes_read < 0) {
    perror("recv");
    return false;
  }
  if (bytes_read == sizeof(packet)) {
    handle_packet(state, &packet);
  } else {
    fprintf(stderr, "warn: Received packet of unexpected size %zd\n",
            bytes_read);
  }
  return true;
}

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

  int wl_fd, net_fd, epoll_fd;
  if ((wl_fd = wl_display_get_fd(display)) < 0
    || (net_fd = net_setup_udp_socket(port)) < 0
    || (epoll_fd = net_setup_epoll(wl_fd, net_fd)) < 0) {
    failure = true;
    goto err_free_all;
  }

  printf("Listening for mouse movements on UDP port %hu...\n", port);
  struct epoll_event events[2];
  while (!quit) {
    wl_display_dispatch_pending(display);
    wl_display_flush(display);
    const int ready = epoll_wait(epoll_fd, events, 2, -1);
    if (ready < 0) {
      if (!quit) {
        perror("epoll_wait");
      }
      break;
    }
    for (int i = 0; i < ready; ++i) {
      const int event_fd = events[i].data.fd;
      if ((event_fd == wl_fd && wl_display_dispatch(display) < 0) ||
          (event_fd == net_fd && !net_receive_packet(&state, net_fd))) {
        break;
      }
    }
  }
  // Cleanup
  close(net_fd);
  close(epoll_fd);
err_free_all:
  zwlr_virtual_pointer_v1_destroy(state.virtual_pointer);
  zwlr_virtual_pointer_manager_v1_destroy(state.pointer_manager);
  wl_seat_destroy(state.seat);
err_free_registry:
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  printf("Shutting down.\n");
  return failure ? EXIT_FAILURE : EXIT_SUCCESS;
}
