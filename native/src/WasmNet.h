// BSD sockets and DNS for wasm32-wasi, where wasi-libc has none of them (WASI preview 1 can only use sockets that the
// host has already opened). Each call fails with ENOSYS, so System.Net.Sockets and System.Net.Dns report an ordinary
// socket error to the C# program instead of the runtime failing to build. A host that does have sockets (a JS bridge)
// can replace these.
#if !defined (__WASMNET_H)
#define __WASMNET_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct in_addr { uint32_t s_addr; };
struct sockaddr_in { unsigned short sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
typedef unsigned int socklen_t;
struct hostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };

#define AF_INET 2
#define SOCK_STREAM 1
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }   // wasm is little-endian
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#define WASMNET_FAIL() (errno = ENOSYS, -1)
static inline int socket(int d, int t, int p) { (void)d; (void)t; (void)p; return WASMNET_FAIL(); }
static inline int bind(int s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return WASMNET_FAIL(); }
static inline int listen(int s, int b) { (void)s; (void)b; return WASMNET_FAIL(); }
static inline int accept(int s, struct sockaddr *a, socklen_t *l) { (void)s; (void)a; (void)l; return WASMNET_FAIL(); }
static inline int connect(int s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return WASMNET_FAIL(); }
static inline long recv(int s, void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; return WASMNET_FAIL(); }
static inline long send(int s, const void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; return WASMNET_FAIL(); }
static inline struct hostent *gethostbyname(const char *n) { (void)n; return NULL; }

#endif
