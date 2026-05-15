#include "virtual_pointer.h"

#include "common.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

void vp_handle_packet(const struct client_state *state,
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

const struct wl_registry_listener vp_registry_listener = {
    .global = &registry_handle_global,
    .global_remove = &registry_handle_global_remove,
};
