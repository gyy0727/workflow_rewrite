#include "poller.h"
#include "list.h"
#include "rbtree.h"
#include <cstddef>
#include <errno.h>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#define POLLER_BUFSIZE (256 * 1024) //*缓冲区上限
#define POLLER_EVENTS_MAX 256       //*监听事件的 上限

//*代表一个文件描述符及其所属事件
struct __poller_node {
  int state;               //*状态
  int error;               //*错误码
  struct poller_data data; //*节点包含的一些列操作
#pragma pack(1)            //*严格字节对齐,填充字节
  union {
    struct list list;
    struct rb_node rb;
  };
#pragma pack()
  char in_rbtree;            //*是否存入红黑树
  char removed;              //*是否已经被移除
  int event;                 //*要监听的事件
  struct timespec timeout;   //*超时时间
  struct __poller_node *res; //*处理结果
};

struct poller {
  size_t max_open_files; //*最大打开的文件描述符数量
  void (*callback)(struct poller_result *, void *); //*回调函数
  void *context;                                    //*上下文
  pthread_t tid;             //*线程ID,每个poller对应了一个线程
  int pfd;                   //*epollfd
  int timerfd;               //*超时文件描述符,每个poller对应一个
  int pipe_rd;               //*用于通知,从epoll_wait()逃逸
  int pipe_wr;               //*同上
  int stopped;               //*是否已经暂停
  struct rb_root timeo_tree; //*装有包含超时时间的节点
  struct rb_node *tree_first; //*超时红黑树的第一个节点,超时时间最小的节点
  struct rb_node *tree_last; //*超时红黑树的最后一个节点,超时时间最大的节点
  struct list timeo_list;       //*已经超时的节点的链表
  struct list no_timeo_list;    //*未超时的节点的链表
  struct __poller_node **nodes; //*node数组,所有本poller监听的节点
  std::mutex mutex;             //*互斥锁
  char buf[POLLER_BUFSIZE];     //*缓冲区
};

//*创建epollfd
static inline int __poller_create_pfd() { return epoll_create(1); }
