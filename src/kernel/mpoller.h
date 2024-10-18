#ifndef _mpollerH_
#define _mpollerH_

#include <stddef.h>
#include "poller.h"

typedef struct mpoller;



mpoller *mpollercreate(const struct poller_params *params, size_t nthreads);
int mpollerstart(mpoller *mpoller);
void mpollerstop(mpoller *mpoller);
void mpollerdestroy(mpoller *mpoller);



struct mpoller
{
	void **nodes_buf;
	unsigned int nthreads;
	poller *poller[1];
};

static inline int mpolleradd(const struct poller_data *data, int timeout,
							  mpoller *mpoller)
{
	int index = (unsigned int)data->fd % mpoller->nthreads;
	return poller_add(data, timeout, mpoller->poller[index]);
}

static inline int mpollerdel(int fd, mpoller *mpoller)
{
	int index = (unsigned int)fd % mpoller->nthreads;
	return poller_del(fd, mpoller->poller[index]);
}

static inline int mpollermod(const struct poller_data *data, int timeout,
							  mpoller *mpoller)
{
	int index = (unsigned int)data->fd % mpoller->nthreads;
	return poller_mod(data, timeout, mpoller->poller[index]);
}

static inline int mpollerset_timeout(int fd, int timeout, mpoller *mpoller)
{
	int index = (unsigned int)fd % mpoller->nthreads;
	return poller_set_timeout(fd, timeout, mpoller->poller[index]);
}

static inline int mpolleradd_timer(const struct timespec *value, void *context,
									void **timer, int *index,
									mpoller *mpoller)
{
	static unsigned int n = 0;
	*index = n++ % mpoller->nthreads;
	return poller_add_timer(value, context, timer, mpoller->poller[*index]);
}

static inline int mpollerdel_timer(void *timer, int index, mpoller *mpoller)
{
	return poller_del_timer(timer, mpoller->poller[index]);
}



#endif