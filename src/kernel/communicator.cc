#include "communicator.h"
#include "list.h"
#include "mpoller.h"
#include "msgqueue.h"
#include "poller.h"
#include "thrdpool.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

struct CommConnEntry {
  struct list list_;
  CommConnection *conn;
  long long seq;
  int sockfd;
#define CONN_STATE_CONNECTING 0
#define CONN_STATE_CONNECTED 1
#define CONN_STATE_RECEIVING 2
#define CONN_STATE_SUCCESS 3
#define CONN_STATE_IDLE 4
#define CONN_STATE_KEEPALIVE 5
#define CONN_STATE_CLOSING 6
#define CONN_STATE_ERROR 7
  int state;
  int error;
  int ref;
  struct iovec *write_iov;
  CommSession *session;
  CommTarget *target;
  CommService *service;
  mpoller *mpoller_;
  /* Connection entry's mutex is for client session only. */
  pthread_mutex_t mutex;
};
