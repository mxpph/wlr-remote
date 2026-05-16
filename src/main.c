#include "common.h"
#include "network.h"
#include "setup.h"
#include "virtual_pointer.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

#include "dtls.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <wayland-client.h>

int main(const int argc, const char **argv) {
  bool failure = false;
  const unsigned short port = setup_parse_port(argc, argv);
  setup_signals();

  char psk_key[PSK_BUFFER_SIZE];
  const size_t psk_key_len = setup_password(psk_key);

  struct client_state state = {
      .psk_key = psk_key,
      .psk_key_len = psk_key_len,
      .sock_v4 = -1,
      .sock_v6 = -1,
  };
  struct wl_display *display = wl_display_connect(NULL);
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &vp_registry_listener, &state);
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

  int wl_fd, epoll_fd;
  if ((wl_fd = wl_display_get_fd(display)) < 0 ||
      (net_setup_udp_sockets(&state, port)) < 0 ||
      (epoll_fd = net_setup_epoll(&state, wl_fd)) < 0) {
    failure = true;
    goto err_free_virtual_pointer;
  }

  dtls_init();
  dtls_context_t *dtls_context = dtls_new_context(&state);
  if (!dtls_context) {
    fprintf(stderr, "error: Failed to create DTLS context\n");
    goto err_free_virtual_pointer;
  }
  dtls_set_handler(dtls_context, &net_dtls_callbacks);

  printf("Listening for mouse movements on UDP port %hu...\n", port);
  struct epoll_event events[3];
  while (!quit) {
    wl_display_dispatch_pending(display);
    wl_display_flush(display);
    const int ready =
        epoll_wait(epoll_fd, events, sizeof(events) / sizeof(*events), -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("epoll_wait");
      goto err_free_all;
    }
    for (int i = 0; i < ready; ++i) {
      const int event_fd = events[i].data.fd;
      if (event_fd == wl_fd) {
        if (wl_display_dispatch(display) < 0) {
          goto err_free_all;
        }
      } else { // either sock_v4 or sock_v6
        if (net_handle_epoll(dtls_context, event_fd) < 0) {
          goto err_free_all;
        }
      }
    }
  }
  // Cleanup
err_free_all:
  dtls_free_context(dtls_context);
  if (state.sock_v4 != -1) {
    close(state.sock_v4);
  }
  if (state.sock_v6 != -1) {
    close(state.sock_v6);
  }
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
