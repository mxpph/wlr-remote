#ifndef WLR_REMOTE_SETUP_H
#define WLR_REMOTE_SETUP_H

#include <crypto.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 39076
#endif

#define PSK_BUFFER_SIZE ((DTLS_PSK_MAX_KEY_LEN) + 2)

void setup_signals(void);
size_t setup_password(char *psk_key);
unsigned short setup_parse_port(int argc, const char **argv);

extern volatile bool quit;

#endif // WLR_REMOTE_SETUP_H
