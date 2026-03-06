#ifndef NETSERVER_H
#define NETSERVER_H

struct mGUIRunner;

/* Start the HTTP server on the given port (background thread).
 * Call after ROM is loaded and romprofileDetect() has run. */
int netserverStart(int port);

/* Stop the server and clean up sockets/thread. */
void netserverStop(void);

/* Snapshot current party data from GBA memory.
 * Call once per frame from the main thread while a game is running. */
void netserverUpdateParty(struct mGUIRunner* runner);

#endif
