#ifndef SERVER_H
#define SERVER_H

#include "nexdb.h"

/* Start a persistent server that listens for JSON-over-TCP connections on the
 * given port.  This function does not return until the server is shut down. */
int server_run(DB *db, int port, const char *unix_path,
               const char *token, int session_ttl);

/* Connect to a running server as a client REPL.
 * address is "host:port" or a Unix socket path. */
int server_connect_repl(const char *address);

#endif /* SERVER_H */
