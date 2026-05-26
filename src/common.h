#ifndef WLR_REMOTE_COMMON_H
#define WLR_REMOTE_COMMON_H

#include "network.h"

#include <stddef.h>
#include <stdint.h>

struct wl_seat;
struct zwlr_virtual_pointer_manager_v1;
struct zwlr_virtual_pointer_v1;

struct client_state {
  struct wl_seat *seat;
  struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
  struct zwlr_virtual_pointer_v1 *virtual_pointer;
  struct sockets socks;
  char *const psk_key;
  const size_t psk_key_len;
};

struct mouse_packet {
  int32_t dx;
  int32_t dy;
  uint16_t button;
  uint16_t button_state;
};

#endif // WLR_REMOTE_COMMON_H
