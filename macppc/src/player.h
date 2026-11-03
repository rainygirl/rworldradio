/* Streams and decodes one internet radio station at a time, entirely in-process.
 * Port of the Rust port's player.rs, which replaced the Haiku build's
 * RadioPlayer (BMediaFile / BMediaTrack / BSoundPlayer).
 *
 * All setup and teardown happens off the caller's thread and status changes are
 * reported asynchronously, so every function here is safe to call from the
 * Carbon event loop.
 *
 * The generation counter that makes a superseded session tear itself down is
 * kept from the original: starting a station while another is connecting bumps
 * the counter, and the old session notices and exits without touching the status
 * line the new one now owns. */
#ifndef RWR_PLAYER_H
#define RWR_PLAYER_H

#include <stddef.h>

#include "dataset.h"

typedef struct Player Player;

typedef enum {
    RWR_EVENT_CONNECTING,
    RWR_EVENT_PLAYING,
    RWR_EVENT_STOPPED,
    RWR_EVENT_ERROR
} PlayerEventKind;

typedef struct {
    PlayerEventKind kind;
    char            station[256];
    char            detail[512];   /* only for RWR_EVENT_ERROR */
} PlayerEvent;

/* `wake` is called from a worker thread after every event is queued, so the UI
 * can pull events promptly instead of waiting for its next timer tick. It must
 * be safe to call from any thread - in this app it posts a Carbon event to the
 * main event queue. */
Player *player_new(void (*wake)(void *context), void *context);
void    player_free(Player *player);

/* Stops whatever is playing and starts connecting to `station`, whose strings
 * are copied. Returns immediately. */
void player_play(Player *player, const Station *station);

/* Safe to call when nothing is playing. */
void player_stop(Player *player);

/* True between play() and the session ending. */
int player_is_active(Player *player);

/* Peak amplitude (0.0-1.0) of the most recently played buffer, or 0 when
 * nothing is playing. Cheap enough to poll from the UI every 100ms. */
float player_level(Player *player);

/* Dequeues one pending event. Returns 1 if `out` was filled, 0 if none. */
int player_poll_event(Player *player, PlayerEvent *out);

#endif
