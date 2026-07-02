#include "advertise.h"

#include "network.h"

#include "mdns.h"

#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>

#define BIT_IPV4_FOUND 0b01
#define BIT_IPV6_FOUND 0b10

static bool create_mdns_sockets(struct sockets *socks,
                                struct interface_addrs *addrs, int found) {
  const unsigned short mdns_port = htons(MDNS_PORT);
  bool success = false;
  if (found & BIT_IPV4_FOUND) {
    addrs->v4.sin_port = mdns_port;
    socks->v4 = mdns_socket_open_ipv4(&addrs->v4);
    success |= socks->v4 != -1;
  }
  if (found & BIT_IPV6_FOUND) {
    addrs->v6.sin6_port = mdns_port;
    socks->v6 = mdns_socket_open_ipv6(&addrs->v6);
    success |= socks->v6 != -1;
  };
  return success;
}

static int get_interfaces(struct interface_addrs *addrs) {
  struct ifaddrs *ifaddr = NULL;
  if (getifaddrs(&ifaddr) < 0) {
    perror("getifaddrs");
    return false;
  }
  unsigned char found = 0;
  for (struct ifaddrs *ifa = ifaddr; ifa != NULL && found != 0b11;
       ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || !(ifa->ifa_flags & IFF_UP) ||
        !(ifa->ifa_flags & IFF_MULTICAST) || (ifa->ifa_flags & IFF_LOOPBACK) ||
        (ifa->ifa_flags & IFF_POINTOPOINT)) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
      if (!(found & BIT_IPV4_FOUND)) {
        addrs->v4 = *sa;
        found |= BIT_IPV4_FOUND;
      }
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
      if (!(found & BIT_IPV6_FOUND) &&
          !IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr) &&
          !IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
        addrs->v6 = *sa6;
        found |= BIT_IPV6_FOUND;
      }
    }
  }
  freeifaddrs(ifaddr);
  return found;
}

static bool create_service_strings(advertise_service_t *service) {
  char *hostname_buffer, *service_instance_buffer, *qualified_hostname_buffer;
  hostname_buffer = service_instance_buffer = qualified_hostname_buffer = NULL;
  // <hostname>
  hostname_buffer = malloc(HOST_NAME_MAX + 1);
  if (!hostname_buffer) {
    return false;
  }
  if (gethostname(hostname_buffer, HOST_NAME_MAX + 1) < 0) {
    perror("gethostname");
    free(hostname_buffer);
    return false;
  }
  size_t hostname_len = strlen(hostname_buffer);
  service->hostname = (mdns_string_t){hostname_buffer, hostname_len};
  service->service_name = (mdns_string_t){MDNS_STRING_CONST(SERVICE_NAME)};
  // <hostname>._wlr-remote._udp.local.
  service_instance_buffer = malloc(HOST_NAME_MAX + 1 + sizeof(SERVICE_NAME));
  if (!service_instance_buffer) {
    return false;
  }
  snprintf(service_instance_buffer, HOST_NAME_MAX + 1 + sizeof(SERVICE_NAME),
           "%s." SERVICE_NAME, hostname_buffer);
  service->service_instance = (mdns_string_t){
      service_instance_buffer, hostname_len + 1 + sizeof(SERVICE_NAME) - 1};
  // <hostname>.local.
  qualified_hostname_buffer = malloc(HOST_NAME_MAX + sizeof(QUALIFIED_SUFFIX));
  if (!qualified_hostname_buffer) {
    return false;
  }
  snprintf(qualified_hostname_buffer, HOST_NAME_MAX + sizeof(QUALIFIED_SUFFIX),
           "%s" QUALIFIED_SUFFIX, hostname_buffer);
  service->qualified_hostname = (mdns_string_t){
      qualified_hostname_buffer, hostname_len + sizeof(QUALIFIED_SUFFIX) - 1};
  return true;
}

static void create_mdns_records(advertise_service_t *service) {
  // Values taken from RFC 6762/6763 recommendations
  service->record_ptr = (mdns_record_t){
      .name = service->service_name,
      .type = MDNS_RECORDTYPE_PTR,
      .data.ptr.name = service->service_instance,
      .rclass = MDNS_CLASS_IN,
      .ttl = 4500,
  };
  service->record_srv = (mdns_record_t){
      .name = service->service_instance,
      .type = MDNS_RECORDTYPE_SRV,
      .data.srv.name = service->qualified_hostname,
      .data.srv.port = service->port,
      .data.srv.priority = 0,
      .data.srv.weight = 0,
      .rclass = MDNS_CLASS_IN,
      .ttl = 120,
  };
  service->record_a = (mdns_record_t){
      .name = service->qualified_hostname,
      .type = MDNS_RECORDTYPE_A,
      .data.a.addr = service->addrs.v4,
      .rclass = MDNS_CLASS_IN,
      .ttl = 120,
  };
  service->record_aaaa = (mdns_record_t){
      .name = service->qualified_hostname,
      .type = MDNS_RECORDTYPE_AAAA,
      .data.aaaa.addr = service->addrs.v6,
      .rclass = MDNS_CLASS_IN,
      .ttl = 120,
  };
  // Required by RFC 6763 section 6
  service->record_txt = (mdns_record_t){
      .name = service->service_instance,
      .type = MDNS_RECORDTYPE_TXT,
      .data.txt.key = (mdns_string_t){MDNS_STRING_CONST("")},
      // No 'value' because of my modifications to mdns.h
      .rclass = MDNS_CLASS_IN,
      .ttl = 120,
  };
}

static size_t build_additional(mdns_record_t *additional,
                               const advertise_service_t *service,
                               enum mdns_record_type type) {
  size_t count = 0;
  switch (type) {
  case MDNS_RECORDTYPE_PTR:
    additional[count++] = service->record_srv;
    // fall through
  case MDNS_RECORDTYPE_SRV:
  case MDNS_RECORDTYPE_AAAA:
    if (service->addrs.v4.sin_family == AF_INET) {
      additional[count++] = service->record_a;
    }
    // fall through
  case MDNS_RECORDTYPE_A:
    if (type != MDNS_RECORDTYPE_AAAA &&
        service->addrs.v6.sin6_family == AF_INET6) {
      additional[count++] = service->record_aaaa;
    }
    // fall through
  case MDNS_RECORDTYPE_TXT:
    additional[count++] = service->record_txt;
    break;
  default:
    fprintf(stderr, "error: Unexpected type in build_additional\n");
  }
  return count;
}

static inline int string_equals(mdns_string_t name, mdns_string_t target) {
  return (name.length == target.length &&
          !strncmp(name.str, target.str, name.length));
}

static int service_callback(int sock, const struct sockaddr *from,
                            size_t addrlen, mdns_entry_type_t entry,
                            uint16_t query_id, uint16_t rtype, uint16_t rclass,
                            uint32_t ttl, const void *data, size_t size,
                            size_t name_offset, size_t name_length,
                            size_t record_offset, size_t record_length,
                            void *user_data) {
  static char name_buffer[256], send_buffer[1024];
  if (entry != MDNS_ENTRYTYPE_QUESTION) {
    return 0;
  }
  const advertise_service_t *service = (const advertise_service_t *)user_data;
  mdns_string_t name = mdns_string_extract(data, size, &(size_t){name_offset},
                                           name_buffer, sizeof(name_buffer));
  mdns_record_t answer;
  mdns_record_t additional[4] = {0};
  size_t additional_count = 0;
  bool send = false;
  if (string_equals(
          name, (mdns_string_t){MDNS_STRING_CONST(DNS_SD_SERVICE_INSTANCE)})) {
    if (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) {
      answer = (mdns_record_t){
          .name = name,
          .type = MDNS_RECORDTYPE_PTR,
          .data.ptr.name = service->service_name,
      };
      send = true;
    }
  } else if (string_equals(name, service->service_name)) {
    if (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) {
      answer = service->record_ptr;
      additional_count =
          build_additional(additional, service, MDNS_RECORDTYPE_PTR);
      send = true;
    }
  } else if (string_equals(name, service->service_instance)) {
    if (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY) {
      answer = service->record_srv;
      additional_count =
          build_additional(additional, service, MDNS_RECORDTYPE_SRV);
      send = true;
    }
  } else if (string_equals(name, service->qualified_hostname)) {
    if ((rtype == MDNS_RECORDTYPE_A || rtype == MDNS_RECORDTYPE_ANY) &&
        service->addrs.v4.sin_family == AF_INET) {
      answer = service->record_a;
      additional_count =
          build_additional(additional, service, MDNS_RECORDTYPE_A);
      send = true;
    } else if ((rtype == MDNS_RECORDTYPE_AAAA ||
                rtype == MDNS_RECORDTYPE_ANY) &&
               service->addrs.v6.sin6_family == AF_INET6) {
      answer = service->record_aaaa;
      additional_count =
          build_additional(additional, service, MDNS_RECORDTYPE_AAAA);
      send = true;
    }
  }
  if (send) {
    if ((rclass & MDNS_UNICAST_RESPONSE)) {
      mdns_query_answer_unicast(sock, from, addrlen, send_buffer,
                                sizeof(send_buffer), query_id, rtype, name.str,
                                name.length, answer, NULL, 0, additional,
                                additional_count);
    } else {
      mdns_query_answer_multicast(sock, send_buffer, sizeof(send_buffer),
                                  answer, NULL, 0, additional,
                                  additional_count);
    }
  }
  return 0;
}

static void advertise_announce(advertise_service_t *service,
                               mdns_socket_callback_fn callback) {
  mdns_record_t additional[4] = {0};
  size_t additional_count =
      build_additional(additional, service, MDNS_RECORDTYPE_PTR);
  struct sockets *socks = &service->socks;
  if (socks->v4 != -1) {
    callback(socks->v4, service->buffer, ADVERTISE_BUFFER_CAPACITY,
             service->record_ptr, NULL, 0, additional, additional_count);
  }
  if (socks->v6 != -1) {
    callback(socks->v6, service->buffer, ADVERTISE_BUFFER_CAPACITY,
             service->record_ptr, NULL, 0, additional, additional_count);
  }
}

advertise_service_t *advertise_create_service(unsigned short port) {
  advertise_service_t *service = malloc(sizeof(*service));
  if (!service) {
    return NULL;
  }
  memset(service, 0, sizeof(*service));
  service->port = port;
  service->socks.v4 = service->socks.v6 = -1;
  int found = get_interfaces(&service->addrs);
  if (!found) {
    fprintf(stderr, "warning: Could not find valid network interface for "
                    "mDNS advertisement, manual "
                    "connection will be required\n");
    goto err_free_service;
  }
  if (!create_mdns_sockets(&service->socks, &service->addrs, found)) {
    fprintf(stderr,
            "warning: Could not create any sockets for mDNS advertisement, "
            "manual connection will be required\n");
    goto err_free_service;
  }
  if (!create_service_strings(service)) {
    fprintf(stderr, "warning: Couldn't create mDNS service strings, manual "
                    "connection will be required\n");
    goto err_free_service;
  };
  create_mdns_records(service);
  service->buffer = malloc(ADVERTISE_BUFFER_CAPACITY);
  if (service->buffer != NULL)
    return service;
err_free_service:
  advertise_destroy_service(service);
  return NULL;
}

bool advertise_setup_epoll(advertise_service_t *service, int epoll_fd) {
  const struct sockets *socks = &service->socks;
  struct epoll_event ev;
  ev.events = EPOLLIN;
  if (socks->v4 != -1) {
    ev.data.fd = socks->v4;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socks->v4, &ev) < 0) {
      perror("epoll_ctl");
      return false;
    }
  }
  if (socks->v6 != -1) {
    ev.data.fd = socks->v6;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socks->v6, &ev) < 0) {
      perror("epoll_ctl");
      return false;
    }
  }
  return true;
}

void advertise_announce_service(advertise_service_t *service) {
  advertise_announce(service, &mdns_announce_multicast);
}

void advertise_announce_goodbye(advertise_service_t *service) {
  advertise_announce(service, &mdns_goodbye_multicast);
}

void advertise_respond_request(advertise_service_t *service, int sock) {
  mdns_socket_listen(sock, service->buffer, ADVERTISE_BUFFER_CAPACITY,
                     &service_callback, service);
}

void advertise_destroy_service(advertise_service_t *service) {
  if (!service) {
    return;
  }
  struct sockets *socks = &service->socks;
  if (socks->v4 != -1) {
    mdns_socket_close(socks->v4);
  }
  if (socks->v6 != -1) {
    mdns_socket_close(socks->v6);
  }
  free((void *)service->hostname.str);
  free((void *)service->service_instance.str);
  free((void *)service->qualified_hostname.str);
  free(service->buffer);
  free(service);
}
