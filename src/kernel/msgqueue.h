

#ifndef _MSGQUEUE_H_
#define _MSGQUEUE_H_

#include <stddef.h>

struct msgqueue;
msgqueue *msgqueue_create(size_t maxlen, int linkoff);
void *msgqueue_get(msgqueue *queue);
void msgqueue_put(void *msg, msgqueue *queue);
void msgqueue_put_head(void *msg, msgqueue *queue);
void msgqueue_set_nonblock(msgqueue *queue);
void msgqueue_set_block(msgqueue *queue);
void msgqueue_destroy(msgqueue *queue);

#endif