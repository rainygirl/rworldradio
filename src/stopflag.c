#include "stopflag.h"

#include <pthread.h>
#include <stdlib.h>

struct StopFlag {
    volatile int    stopped;
    pthread_mutex_t lock;   /* guards the refcount only, never `stopped` */
    int             refcount;
};

StopFlag *stopflag_new(void)
{
    StopFlag *flag = (StopFlag *)calloc(1, sizeof(*flag));

    if (!flag)
        return NULL;
    pthread_mutex_init(&flag->lock, NULL);
    flag->refcount = 1;
    return flag;
}

StopFlag *stopflag_retain(StopFlag *flag)
{
    if (!flag)
        return NULL;
    pthread_mutex_lock(&flag->lock);
    flag->refcount++;
    pthread_mutex_unlock(&flag->lock);
    return flag;
}

void stopflag_release(StopFlag *flag)
{
    int dead;

    if (!flag)
        return;
    pthread_mutex_lock(&flag->lock);
    dead = (--flag->refcount == 0);
    pthread_mutex_unlock(&flag->lock);
    if (!dead)
        return;
    pthread_mutex_destroy(&flag->lock);
    free(flag);
}

void stopflag_set(StopFlag *flag)
{
    if (flag)
        flag->stopped = 1;
}

int stopflag_is_set(const StopFlag *flag)
{
    return flag ? flag->stopped : 1;
}
