#ifndef WLR_REMOTE_NETWORK_H
#define WLR_REMOTE_NETWORK_H

#include "dtls.h"

struct client_state;

int net_setup_udp_sockets(struct client_state *state, unsigned short port);
int net_setup_epoll(const struct client_state *state, int wl_fd);
int net_handle_epoll(dtls_context_t *ctx, int sock_fd);

extern dtls_handler_t net_dtls_callbacks;

#endif // WLR_REMOTE_NETWORK_H
