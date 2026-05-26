#ifndef WLR_REMOTE_ADVERTISE_H
#define WLR_REMOTE_ADVERTISE_H

#include "network.h"

#include "mdns.h"

#include <stdbool.h>

#define QUALIFIED_SUFFIX ".local."
#define SERVICE_NAME "_wlr-remote._udp.local."
#define DNS_SD_SERVICE_INSTANCE "_services._dns-sd._udp.local."

#define ADVERTISE_BUFFER_CAPACITY 2048

struct interface_addrs {
  struct sockaddr_in v4;
  struct sockaddr_in6 v6;
};

typedef struct {
  mdns_string_t service_name;
  mdns_string_t hostname;
  mdns_string_t service_instance;
  mdns_string_t qualified_hostname;
  struct interface_addrs addrs;
  struct sockets socks;
  unsigned short port;
  mdns_record_t record_ptr;
  mdns_record_t record_srv;
  mdns_record_t record_a;
  mdns_record_t record_aaaa;
  mdns_record_t record_txt;
  void *buffer;
} advertise_service_t;

advertise_service_t *advertise_create_service(unsigned short port);
bool advertise_setup_epoll(advertise_service_t *service, int epoll_fd);
void advertise_announce_service(advertise_service_t *service);
void advertise_announce_goodbye(advertise_service_t *service);
void advertise_respond_request(advertise_service_t *service, int sock);
void advertise_destroy_service(advertise_service_t *service);

#endif
