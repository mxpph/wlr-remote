#include "network.h"

#include "common.h"
#include "virtual_pointer.h"

#include "dtls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static const unsigned char PSK_ID[] = "wlr-remote";

int net_setup_udp_sockets(struct client_state *state,
                          const unsigned short port) {
  const struct addrinfo hints = {
      .ai_family = AF_UNSPEC,
      .ai_flags = AI_PASSIVE,
      .ai_socktype = SOCK_DGRAM,
      .ai_protocol = IPPROTO_UDP,
  };
  struct addrinfo *head;
  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%hu", port);

  int gai_error;
  if ((gai_error = getaddrinfo(NULL, port_str, &hints, &head) != 0)) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_error));
    return -1;
  }

  for (struct addrinfo *info = head;
       info != NULL && (state->sock_v4 == -1 || state->sock_v6 == -1);
       info = info->ai_next) {
    const int sock =
        socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (sock < 0) {
      perror("socket");
      continue;
    }
    if (info->ai_family == AF_INET6 &&
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &(int){1}, sizeof(int)) <
            0) {
      perror("setsockopt IPV6_V6ONLY");
      close(sock);
    }

    if (bind(sock, info->ai_addr, info->ai_addrlen) < 0) {
      perror("bind");
      close(sock);
      continue;
    }
    if (info->ai_family == AF_INET && state->sock_v4 == -1) {
      state->sock_v4 = sock;
    } else if (info->ai_family == AF_INET6 && state->sock_v6 == -1) {
      state->sock_v6 = sock;
    } else {
      close(sock);
    }
  }
  freeaddrinfo(head);

  if (state->sock_v4 == -1 && state->sock_v6 == -1) {
    fprintf(stderr, "error: Failed to bind any sockets\n");
    return -1;
  }
  return 0;
}

int net_setup_epoll(const struct client_state *state, const int wl_fd) {
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
  if (state->sock_v4 != -1) {
    ev.data.fd = state->sock_v4;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state->sock_v4, &ev) < 0) {
      goto err_close_fds;
    }
  }
  if (state->sock_v6 != -1) {
    ev.data.fd = state->sock_v6;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state->sock_v6, &ev) < 0) {
      goto err_close_fds;
    }
  }
  return epoll_fd;
err_close_fds:
  perror("epoll_ctl");
  if (state->sock_v4 != -1) {
    close(state->sock_v4);
  }
  if (state->sock_v6 != -1) {
    close(state->sock_v6);
  }
  close(epoll_fd);
  return -1;
}

int net_handle_epoll(dtls_context_t *ctx, const int sock_fd) {
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
  int sock;
  if (session->addr.sa.sa_family == AF_INET) {
    sock = state->sock_v4;
  } else if (session->addr.sa.sa_family == AF_INET6) {
    sock = state->sock_v6;
  } else {
    fprintf(stderr, "error: unrecognized address family for outbound packet\n");
    return -1;
  }
  const ssize_t sent =
      sendto(sock, buf, len, MSG_DONTWAIT, &session->addr.sa, session->size);
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
    vp_handle_packet(state, &packet);
  } else {
    fprintf(stderr, "warn: Received packet of unexpected size %zu\n", len);
  }
  return 0;
}

static int net_handle_event(dtls_context_t *ctx, session_t *session,
                            dtls_alert_level_t level, unsigned short code) {
  char client_addr[INET6_ADDRSTRLEN];
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
  if (type != DTLS_PSK_KEY) { // don't send a hint
    return 0;
  }

  if (id_len == sizeof(PSK_ID) - 1 && !memcmp(id, PSK_ID, sizeof(PSK_ID) - 1)) {
    const struct client_state *state =
        (struct client_state *)dtls_get_app_data(ctx);
    if (result_length < state->psk_key_len) {
      return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
    }
    memcpy(result, state->psk_key, state->psk_key_len);
    return (int)state->psk_key_len;
  }
  return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
}

dtls_handler_t net_dtls_callbacks = {
    .write = &net_handle_send,
    .read = &net_handle_receive,
    .event = &net_handle_event,
    .get_psk_info = &net_get_psk_info,
};
