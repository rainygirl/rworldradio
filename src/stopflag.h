/* A refcounted "stop now" flag, shared between the session thread, the HTTP
 * stream worker and the HLS worker.
 *
 * This is the Rust ports' `Arc<AtomicBool>`. It has to be refcounted rather than
 * a plain pointer into the session: the network workers are detached threads
 * that may still be unwinding after the session that started them is gone, and
 * they read the flag on every loop iteration.
 *
 * The flag itself is a plain int written once (0 -> 1) and never cleared, so it
 * needs no lock: the worst a torn read could do is take one more pass around a
 * loop, and PPC and every other target here store an aligned int atomically.
 * `volatile` is what stops the compiler hoisting the read out of those loops. */
#ifndef RWR_STOPFLAG_H
#define RWR_STOPFLAG_H

typedef struct StopFlag StopFlag;

StopFlag *stopflag_new(void);
StopFlag *stopflag_retain(StopFlag *flag);
void      stopflag_release(StopFlag *flag);

void stopflag_set(StopFlag *flag);
int  stopflag_is_set(const StopFlag *flag);

#endif
