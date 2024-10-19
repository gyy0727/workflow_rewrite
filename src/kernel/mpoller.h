#ifndef _mpollerH_
#define _mpollerH_

#include "poller.h"
#include <stddef.h>

struct mpoller;

mpoller *mpoller_create(const struct poller_params *params, size_t nthreads);
int mpoller_start(mpoller *mpoller);
void mpoller_stop(mpoller *mpoller);
void mpoller_destroy(mpoller *mpoller);

struct mpoller {
  void **nodes_buf;
  unsigned int nthreads;
  poller *poller[1];
};

static inline int mpoller_add(const struct poller_data *data, int timeout,
                              mpoller *mpoller) {
  int index = (unsigned int)data->fd % mpoller->nthreads;
  return poller_add(data, timeout, mpoller->poller[index]);
}

static inline int mpoller_del(int fd, mpoller *mpoller) {
  int index = (unsigned int)fd % mpoller->nthreads;
  return poller_del(fd, mpoller->poller[index]);
}

static inline int mpoller_mod(const struct poller_data *data, int timeout,
                              mpoller *mpoller) {
  int index = (unsigned int)data->fd % mpoller->nthreads;
  return poller_mod(data, timeout, mpoller->poller[index]);
}

static inline int mpoller_set_timeout(int fd, int timeout, mpoller *mpoller) {
  int index = (unsigned int)fd % mpoller->nthreads;
  return poller_set_timeout(fd, timeout, mpoller->poller[index]);
}

static inline int mpoller_add_timer(const struct timespec *value, void *context,
                                    void **timer, int *index,
                                    mpoller *mpoller) {
  static unsigned int n = 0;
  *index = n++ % mpoller->nthreads;
  return poller_add_timer(value, context, timer, mpoller->poller[*index]);
}

static inline int mpoller_del_timer(void *timer, int index, mpoller *mpoller) {
  return poller_del_timer(timer, mpoller->poller[index]);
}

#endif