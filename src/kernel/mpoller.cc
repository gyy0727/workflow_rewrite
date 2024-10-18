#include "mpoller.h"
#include "poller.h"
#include <stddef.h>
#include <stdlib.h>

extern poller *__poller_create(void **, const struct poller_params *);
extern void __poller_destroy(poller *);

static int __mpoller_create(const struct poller_params *params,
                            mpoller *mpoller) {
  void **nodes_buf = (void **)calloc(params->max_open_files, sizeof(void *));
  unsigned int i;

  if (nodes_buf) {
    for (i = 0; i < mpoller->nthreads; i++) {
      mpoller->poller[i] = __poller_create(nodes_buf, params);
      if (!mpoller->poller[i])
        break;
    }

    if (i == mpoller->nthreads) {
      mpoller->nodes_buf = nodes_buf;
      return 0;
    }

    while (i > 0)
      __poller_destroy(mpoller->poller[--i]);

    free(nodes_buf);
  }

  return -1;
}

mpoller *mpoller_create(const struct poller_params *params, size_t nthreads) {
  mpoller *mpoller;
  size_t size;

  if (nthreads == 0)
    nthreads = 1;

  size = offsetof(struct mpoller, poller) + nthreads * sizeof(void *);
  mpoller = (struct mpoller *)malloc(size);
  if (mpoller) {
    mpoller->nthreads = (unsigned int)nthreads;
    if (__mpoller_create(params, mpoller) >= 0)
      return mpoller;

    free(mpoller);
  }

  return NULL;
}

int mpoller_start(mpoller *mpoller) {
  size_t i;

  for (i = 0; i < mpoller->nthreads; i++) {
    if (poller_start(mpoller->poller[i]) < 0)
      break;
  }

  if (i == mpoller->nthreads)
    return 0;

  while (i > 0)
    poller_stop(mpoller->poller[--i]);

  return -1;
}

void mpoller_stop(mpoller *mpoller) {
  size_t i;

  for (i = 0; i < mpoller->nthreads; i++)
    poller_stop(mpoller->poller[i]);
}

void mpoller_destroy(mpoller *mpoller) {
  size_t i;

  for (i = 0; i < mpoller->nthreads; i++)
    __poller_destroy(mpoller->poller[i]);

  free(mpoller->nodes_buf);
  free(mpoller);
}
