#ifndef WLR_REMOTE_VIRTUAL_POINTER_H
#define WLR_REMOTE_VIRTUAL_POINTER_H

struct client_state;
struct mouse_packet;
struct wl_registry;

void vp_handle_packet(const struct client_state *state,
                      const struct mouse_packet *packet);

extern const struct wl_registry_listener vp_registry_listener;

#endif // WLR_REMOTE_VIRTUAL_POINTER_H
