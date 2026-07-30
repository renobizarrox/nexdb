#ifndef SERVER_H
#define SERVER_H

#include "nexdb.h"

/* Start a persistent server that listens for JSON-over-TCP connections on the
 * given port.  This function does not return until the server is shut down.
 * tls_cert and tls_key are optional — when both are non-NULL the server
 * wraps accepted connections with TLS. */
int server_run(DB *db, int port, const char *unix_path,
               const char *token, int session_ttl,
               const char *tls_cert, const char *tls_key);

/* Connect to a running server as a client REPL.
 * address is "host:port" or a Unix socket path. */
int server_connect_repl(const char *address);

#endif /* SERVER_H */
