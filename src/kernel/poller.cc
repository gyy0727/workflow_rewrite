#include "poller.h"
#include "list.h"
#include "rbtree.h"
#include <bits/types/struct_iovec.h>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
  struct list *timeo_list;      //*已经超时的节点的链表
  struct list *no_timeo_list;   //*未超时的节点的链表
  struct __poller_node **nodes; //*node数组,所有本poller监听的节点
  std::mutex mutex;             //*互斥锁
  char buf[POLLER_BUFSIZE];     //*缓冲区
};

//*创建epollfd
static inline int __poller_create_pfd() { return epoll_create(1); }

//*添加fd到epollfd中
static inline int __poller_add_fd(int fd, int event, void *data,
                                  poller *poller) {
  struct epoll_event ev = {.events = (uint32_t)event, .data = {.ptr = data}};

  return epoll_ctl(poller->pfd, EPOLL_CTL_ADD, fd, &ev);
}

//*从epollfd中删除fd
static inline int __poller_delete_fd(int fd, int event, poller *poller) {
  return epoll_ctl(poller->pfd, EPOLL_CTL_DEL, fd, NULL);
}

//*修改fd的监听事件
static inline int __poller_modify_fd(int fd, int old_event, int new_event,
                                     void *data, poller *poller) {
  struct epoll_event ev = {.events = (uint32_t)new_event,
                           .data = {.ptr = data}};
  return epoll_ctl(poller->pfd, EPOLL_CTL_MOD, fd, &ev);
}

//*创建timerfd
//*CLOCK_MONOTONIC
//*参数表示使用的时钟是单调时钟，它不受系统时间更改的影响，适合用于定时器
static inline int __poller_create_timerfd() {
  return timerfd_create(CLOCK_MONOTONIC, 0);
}

static inline int __poller_close_timerfd(int fd) { return close(fd); }

//*超时文件描述符添加到poller
static inline int __poller_add_timerfd(int fd, poller *poller) {
  struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data = {.ptr = NULL}};
  return epoll_ctl(poller->pfd, EPOLL_CTL_ADD, fd, &ev);
}

//*设置超时文件描述符的超时时间
static inline int __poller_set_timerfd(int fd, const struct timespec *abstime) {
  struct itimerspec timer = {.it_interval = {}, .it_value = *abstime};

  return timerfd_settime(fd, TFD_TIMER_ABSTIME, &timer, NULL);
}

static inline int __poller_wait(epoll_event *events, int maxevents,
                                poller *poller) {
  //*-1表示无限等待,maxevents==sizeof events/sizeof events[0]
  return epoll_wait(poller->pfd, events, maxevents, -1);
}

//*取出epoll_event的data,用于回调
static inline void *__poller_get_event_data(const epoll_event *event) {
  return event->data.ptr;
}

//*比较两个时间点的大小,return node1-node2
static inline long __timeout_cmp(const struct __poller_node *node1,
                                 const struct __poller_node *node2) {
  long ret = node1->timeout.tv_sec - node2->timeout.tv_sec;
  if (ret == 0) {
    ret = node1->timeout.tv_nsec - node2->timeout.tv_nsec;
  }
  return ret;
}

//*将节点插入到超时红黑树中
static void __poller_tree_insert(struct __poller_node *node, poller *poller) {
  struct rb_node **p = &poller->timeo_tree.rb_node;
  struct rb_node *parent = NULL;
  struct __poller_node *entry;
  //*通过偏移量的计算,得出poller_node的起始地址
  entry = rb_entry(poller->tree_last, struct __poller_node, rb);
  //*超时红黑树为空,直接插入
  if (!*p) {
    poller->tree_first = &node->rb;
    poller->tree_last = &node->rb;
  } else if (__timeout_cmp(node, entry) >= 0) {
    //*node大于超时红黑树的最后一个节点,插入到最后
    parent = poller->tree_last;
    p = &parent->rb_right;
    poller->tree_last = &node->rb;
  } else {
    //*node的超时时间小于超时红黑树超时时间最大的节点
    //*，在这个循环中，它比较新节点 node 的超时时间和当前节点 entry
    //*的超时时间。如果新节点的超时时间小于当前节点的超时时间，那么 p
    //*指针指向当前节点的左子节点；否则，p
    //*指针指向当前节点的右子节点。这个过程一直持续到 p
    //*指针指向的位置为空，即找到了新节点应该插入的位置。
    do {
      parent = *p;
      entry = rb_entry(*p, struct __poller_node, rb);
      if (__timeout_cmp(node, entry) < 0)
        p = &(*p)->rb_left;
      else
        p = &(*p)->rb_right;
    } while (*p);
    //*判断是不是等于最左的节点,如果是就更新tree->first
    if (p == &poller->tree_first->rb_left)
      poller->tree_first = &node->rb;
  }
  //*1代表已插入
  node->in_rbtree = 1;
  //*下面两个操作用于插入节点后调整红黑树
  rb_link_node(&node->rb, parent, p);
  rb_insert_color(&node->rb, &poller->timeo_tree);
}

//*从超时红黑树中删除节点
static inline void __poller_tree_erase(struct __poller_node *node,
                                       poller *poller) {
  if (&node->rb == poller->tree_first)
    poller->tree_first = rb_next(&node->rb);
  if (&node->rb == poller->tree_last)
    poller->tree_last = rb_prev(&node->rb);
  rb_erase(&node->rb, &poller->timeo_tree);
  node->in_rbtree = 0;
}

//*从poller删除节点
static int __poller_remove_node(struct __poller_node *node, poller *poller) {
  int removed;
  removed = node->removed;
  std::unique_lock<std::mutex> lock(poller->mutex);
  //*如果还没删除
  if (!removed) {
    poller->nodes[node->data.fd] = NULL;
    //*不是在rbtree就是list
    if (node->in_rbtree)
      __poller_tree_erase(node, poller);
    else
      list_delete(&node->list);
    //*从epoll中删除fd
    __poller_delete_fd(node->data.fd, node->event, poller);
  }
  return removed;
}

//! 应该是用于将从socket读出的数据搬运到指定地方
static int __poller_append_message(const void *buf, size_t *n,
                                   struct __poller_node *node, poller *poller) {
  poller_message *msg = node->data.message;
  struct __poller_node *res;
  int ret = 0;
  if (!msg) {
    res = (struct __poller_node *)malloc(sizeof(struct __poller_node));
    if (!res) {
      return -1;
    }
    msg = node->data.create_message(node->data.context);
    if (!msg) {
      free(res);
      return -1;
    }
    node->data.message = msg;
    node->res = res;
  } else {
    res = node->res;
  }
  ret = msg->append(buf, n, msg);
  if (ret > 0) {
    res->data = node->data;
    res->error = 0;
    res->state = PR_ST_SUCCESS;
    poller->callback((struct poller_result *)res, poller->context);

    node->data.message = NULL;
    node->res = NULL;
  }
  return ret;
}

//*处理读事件
static void __poller_handle_read(struct __poller_node *node, poller *poller) {
  ssize_t nleft;
  size_t n;
  char *p;

  //*将读出的数据通过append_message函数搬运到指定位置
  while (1) {
    p = poller->buf;
    nleft = read(node->data.fd, p, POLLER_BUFSIZE);
    if (nleft < 0 && errno == EAGAIN)
      return;

    if (nleft <= 0)
      break;

    do {
      n = nleft;
      if (__poller_append_message(p, &n, node, poller) >= 0) {
        nleft -= n;
        p += n;
      } else
        nleft = -1;
    } while (nleft > 0);
    if (nleft < 0) {
      break;
    }
  }
  //*删除节点
  if (__poller_remove_node(node, poller))
    return;

  //*如果为0 ,那就代表是正常读取完的
  if (nleft == 0) {
    node->error = 0;
    node->state = PR_ST_FINISHED;
  } else {
    node->error = errno;
    node->state = PR_ST_ERROR;
  }

  free(node->res);
  poller->callback((struct poller_result *)node, poller->context);
}

//*写事件的处理
static void __poller_handle_write(struct __poller_node *node, poller *poller) {
  struct iovec *iov = node->data.write_iov; //*先指向写缓冲区
  size_t count = 0;                         //*已经发送的写结构体数量
  ssize_t nleft;                            //*每次while循环发送的大小
  int iovcnt;                               //*需要发送iovec的数量
  int ret = 0;

  while (node->data.iovcnt > 0) {
    iovcnt = node->data.iovcnt;
    //*要发送的数据量超过了最大值
    if (iovcnt > IOV_MAX) {
      iovcnt = IOV_MAX;
    }
    nleft = writev(node->data.fd, iov, iovcnt);
    if (nleft < 0) {
      ret = errno == EAGAIN ? 0 : -1;
      break;
    }
    count += nleft;
    do {
      if (nleft >= (ssize_t)iov->iov_len) {
        nleft -= iov->iov_len;
        iov->iov_base = (char *)iov->iov_base + iov->iov_len;
        iov->iov_len = 0;
        iov++; //*指向下一个结构体,write_iov是一个iovec数组
        node->data.iovcnt--; //*减少剩余数量
      } else {
        iov->iov_base = (char *)iov->iov_base + nleft;
        iov->iov_len -= nleft;
        break;
      }
    } while (node->data.iovcnt > 0);
  }

  node->data.write_iov = iov;
  //*没发送完,出错了
  if (node->data.iovcnt > 0 && ret >= 0) {
    if (count == 0) //*一个都没发送
      return;
    if (node->data.partial_written(count, node->data.context) >= 0)
      return;
  }

  //*发送完了,就删除
  if (__poller_remove_node(node, poller))
    return;

  //*删除节点出错
  if (node->data.iovcnt == 0) {
    node->error = 0;
    node->state = PR_ST_FINISHED;
  } else {
    node->error = errno;
    node->state = PR_ST_ERROR;
  }
  poller->callback((struct poller_result *)node, poller->context);
}

//*处理监听事件
static void __poller_handle_listen(struct __poller_node *node, poller *poller) {
  struct __poller_node *res = node->res;
  struct sockaddr_storage ss; //*通用地址
  struct sockaddr *addr = (struct sockaddr *)&ss;
  socklen_t addrlen;
  void *result;
  int socketfd;
  while (1) {
    addrlen = sizeof(struct sockaddr_storage);
    socketfd = accept(node->data.fd, addr, &addrlen);
    //*error
    if (socketfd < 0) {
      //*EAGAIN 表示操作无法立即完成
      //*EMFILE表示当前进程已打开太多文件描述符
      //*ENFILE 表示系统文件描述符已用完
      if (errno == EAGAIN || errno == EMFILE || errno == ENFILE)
        return;
      else if (errno == ECONNABORTED)
        continue;
      else
        break;
    }

    result = node->data.accept(addr, addrlen, socketfd, node->data.context);
    if (!result)
      break;
    res->data = node->data;
    res->data.result = result;
    res->error = 0;
    res->state = PR_ST_SUCCESS;
    poller->callback((struct poller_result *)res, poller->context);

    res = (struct __poller_node *)malloc(sizeof(struct __poller_node));
    node->res = res;
    if (!res)
      break;
  }

  if (__poller_remove_node(node, poller))
    return;

  node->error = errno;
  node->state = PR_ST_ERROR;
  free(node->res);
  poller->callback((struct poller_result *)node, poller->context);
}

//*连接事件
static void __poller_handle_connect(struct __poller_node *node,
                                    poller *poller) {
  socklen_t len = sizeof(int);
  int error;
  //*获取套接字错误
  if (getsockopt(node->data.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
    error = errno;
  //*删除节点
  if (__poller_remove_node(node, poller))
    return;
  if (error == 0) {
    node->error = 0;
    node->state = PR_ST_FINISHED;
  } else {
    node->error = error;
    node->state = PR_ST_ERROR;
  }
  poller->callback((struct poller_result *)node, poller->context);
}

//*recvfrom事件处理
static void __poller_handle_recvfrom(struct __poller_node *node,
                                     poller *poller) {
  struct __poller_node *res = node->res;
  struct sockaddr_storage ss;
  struct sockaddr *addr = (struct sockaddr *)&ss;
  socklen_t addrlen;
  void *result;
  ssize_t n;

  while (1) {
    addrlen = sizeof(struct sockaddr_storage);
    //*之所以读写poller->buf不用加锁,是因为one poller per thread
    n = recvfrom(node->data.fd, poller->buf, POLLER_BUFSIZE, 0, addr, &addrlen);
    if (n < 0) {
      if (errno == EAGAIN)
        return;
      else
        break;
    }

    result =
        node->data.recvfrom(addr, addrlen, poller->buf, n, node->data.context);
    if (!result)
      break;

    res->data = node->data;
    res->data.result = result;
    res->error = 0;
    res->state = PR_ST_SUCCESS;
    poller->callback((struct poller_result *)res, poller->context);

    res = (struct __poller_node *)malloc(sizeof(struct __poller_node));
    node->res = res;
    if (!res)
      break;
  }

  if (__poller_remove_node(node, poller))
    return;

  node->error = errno;
  node->state = PR_ST_ERROR;
  free(node->res);
  poller->callback((struct poller_result *)node, poller->context);
}

//*处理文件的异步IO事件的完成
static void __poller_handle_event(struct __poller_node *node, poller *poller) {
  struct __poller_node *res = node->res;
  unsigned long long cnt = 0; //*cnt代表异步IO事件的数量
  unsigned long long value;
  void *result;
  ssize_t n;

  while (1) {
    n = read(node->data.fd, &value, sizeof(unsigned long long));
    if (n == sizeof(unsigned long long))
      cnt += value;
    else {
      if (n >= 0)
        errno = EINVAL;
      break;
    }
  }

  //*读完了,开始处理异步IO事件
  if (errno == EAGAIN) {
    while (1) {
      if (cnt == 0)
        return;

      cnt--;
      result = node->data.event(node->data.context);
      if (!result)
        break;
      res->data = node->data;
      res->data.result = result;
      res->error = 0;
      res->state = PR_ST_SUCCESS;
      poller->callback((struct poller_result *)res, poller->context);
      res = (struct __poller_node *)malloc(sizeof(struct __poller_node));
      node->res = res;
      if (!res)
        break;
    }
  }

  if (cnt != 0)
    write(node->data.fd, &cnt, sizeof(unsigned long long));

  if (__poller_remove_node(node, poller))
    return;

  node->error = errno;
  node->state = PR_ST_ERROR;
  free(node->res);
  poller->callback((struct poller_result *)node, poller->context);
}

//*pipe通知事件
static int __poller_handle_pipe(poller *poller) {
  struct __poller_node **node = (struct __poller_node **)poller->buf;
  int stop = 0;
  int n;
  int i;

  n = read(poller->pipe_rd, node, POLLER_BUFSIZE) / sizeof(void *);
  for (i = 0; i < n; i++) {
    if (node[i]) {
      free(node[i]->res);
      poller->callback((struct poller_result *)node[i], poller->context);
    } else
      stop = 1;
  }

  return stop;
}

//*处理超时事件,节点超时只会被删除,不会进行任何操作
static inline void
__poller_handle_timeout(const struct __poller_node *time_node, poller *poller) {
  struct __poller_node *node;
  struct list *pos;
  struct list timeo_list; //*存放取出的超时节点
  list_init(&timeo_list);
  std::unique_lock<std::mutex> lock(poller->mutex);
  pos = poller->timeo_list->next;
  while (pos != poller->timeo_list) {
    node = list_entry(pos, struct __poller_node, list);
    //*找到第一个没超时的节点,停止循环
    if (__timeout_cmp(node, time_node) > 0) {
      break;
    }
    //*以下是处理超时的节点
    if (node->data.fd >= 0) {
      poller->nodes[node->data.fd] = NULL; //*将对应的node置为NULL
      __poller_delete_fd(node->data.fd, node->event, poller);
    } else {
      node->removed = 1;
    }

    list_add_tail(timeo_list.prev, pos);
    pos = pos->next;
  }
  //*遍历红黑树取出超时节点
  while (poller->tree_first) {

    node = rb_entry(poller->tree_first, struct __poller_node, rb);
    //*红黑树所有节点都还没超时
    if (__timeout_cmp(node, time_node) > 0)
      break;

    //*从超时红黑树中删除已超时的节点
    if (node->data.fd >= 0) {
      poller->nodes[node->data.fd] = NULL;
      __poller_delete_fd(node->data.fd, node->event, poller);
    } else
      node->removed = 1;
    poller->tree_first = rb_next(poller->tree_first);
    rb_erase(&node->rb, &poller->timeo_tree);
    list_add_tail(timeo_list.prev, &node->list);
    if (!poller->tree_first) {
      poller->tree_last = NULL;
    }
  }
  lock.unlock();
  pos = timeo_list.next;
  while (pos != &timeo_list) {
    node = list_entry(pos, struct __poller_node, list);
    if (node->data.fd >= 0) {
      //*节点超时只会被删除,不会进行任何操作
      node->error = ETIMEDOUT;
      node->state = PR_ST_ERROR;
    } else {
      node->error = 0;
      node->state = PR_ST_FINISHED;
    }
    free(node->res);
    poller->callback((struct poller_result *)node, poller->context);
    pos = pos->next;
  }
}

static void __poller_set_timer(poller *poller) {
  struct __poller_node *node = NULL;
  struct __poller_node *first;
  struct timespec abstime;

  std::unique_lock<std::mutex> lock(poller->mutex);
  if (!list_empty(poller->timeo_list)) //*超时链表不为空
    node = list_entry(poller->timeo_list->next, struct __poller_node, list);
  //*超时红黑树不为空
  if (poller->tree_first) {
    first = rb_entry(poller->tree_first, struct __poller_node, rb);
    //*比较链表的最小超时节点和红黑树的最小超时节点
    if (!node || __timeout_cmp(first, node) < 0)
      node = first;
  }
  if (node)
    abstime = node->timeout;
  else {
    abstime.tv_sec = 0;
    abstime.tv_nsec = 0;
  }
  //*将得出的超时时间设置给poller的timerfd
  __poller_set_timerfd(poller->timerfd, &abstime);
}

static void *__poller_thread_routine(void *args) {
  // Bug 修复：将 (poller *) 改为 (poller *)
  poller *poller = static_cast<struct poller *>(args);
  epoll_event events[POLLER_EVENTS_MAX]; //*用于存放发生事件的结构体
  struct __poller_node time_node;
  struct __poller_node *node;
  int has_pipe_event;
  int nevents;
  int i;

  while (1) {
    __poller_set_timer(poller);
    nevents = __poller_wait(events, POLLER_EVENTS_MAX, poller);
    //*获取当前的时间点存入time_node中
    clock_gettime(CLOCK_MONOTONIC, &time_node.timeout);
    has_pipe_event = 0;
    for (i = 0; i < nevents; i++) {
      node = (struct __poller_node *)__poller_get_event_data(&events[i]);
      if (node <= (struct __poller_node *)1) {
        if (node == (struct __poller_node *)1)
          has_pipe_event = 1;
        continue;
      }

      switch (node->data.operation) {
      case PD_OP_READ:
        __poller_handle_read(node, poller);
        break;
      case PD_OP_WRITE:
        __poller_handle_write(node, poller);
        break;
      case PD_OP_LISTEN:
        __poller_handle_listen(node, poller);
        break;
      case PD_OP_CONNECT:
        __poller_handle_connect(node, poller);
        break;
      case PD_OP_RECVFROM:
        __poller_handle_recvfrom(node, poller);
        break;
      case PD_OP_EVENT:
        __poller_handle_event(node, poller);
        break;
      case PD_OP_NOTIFY:
        // __poller_handle_notify(node, poller);
        break;
      }
    }

    if (has_pipe_event) {
      if (__poller_handle_pipe(poller))
        break;
    }

    __poller_handle_timeout(&time_node, poller);
  }

  return NULL;
}

//*初始化管道文件描述符
static int __poller_open_pipe(poller *poller) {
  int pipefd[2];

  if (pipe(pipefd) >= 0) {
    if (__poller_add_fd(pipefd[0], EPOLLIN, (void *)1, poller) >= 0) {
      poller->pipe_rd = pipefd[0];
      poller->pipe_wr = pipefd[1];
      return 0;
    }

    close(pipefd[0]);
    close(pipefd[1]);
  }

  return -1;
}

//*就是将超时文件描述符加入到epoll中监听
static int __poller_create_timer(poller *poller) {
  int timerfd = __poller_create_timerfd();
  if (timerfd >= 0) {
    if (__poller_add_timerfd(timerfd, poller) >= 0) {
      poller->timerfd = timerfd;
      return 0;
    }
    __poller_close_timerfd(timerfd);
  }

  return -1;
}

poller *__poller_create(void **nodes_buf, const struct poller_params *params) {
  poller *poller = (struct poller *)malloc(sizeof(struct poller));
  int ret = 0;

  if (!poller)
    return NULL;

  poller->pfd = __poller_create_pfd();
  if (poller->pfd >= 0) {
    if (__poller_create_timer(poller) >= 0) {
      if (ret == 0) {
        poller->nodes = (struct __poller_node **)nodes_buf;
        poller->max_open_files = params->max_open_files;
        poller->callback = params->callback;
        poller->context = params->context;

        poller->timeo_tree.rb_node = NULL;
        poller->tree_first = NULL;
        poller->tree_last = NULL;
        list_init(poller->timeo_list);
        list_init(poller->no_timeo_list);
        poller->stopped = 1;
        return poller;
      }

      errno = ret;
      close(poller->timerfd);
    }

    close(poller->pfd);
  }

  free(poller);
  return NULL;
}

poller *poller_create(const struct poller_params *params) {
  void **nodes_buf = (void **)calloc(params->max_open_files, sizeof(void *));
  poller *poller;

  if (nodes_buf) {
    poller = __poller_create(nodes_buf, params);
    if (poller)
      return poller;

    free(nodes_buf);
  }

  return NULL;
}

void __poller_destroy(poller *poller) {
  // pthread_mutex_destroy(&poller->mutex);
  __poller_close_timerfd(poller->timerfd);
  close(poller->pfd);
  free(poller);
}

void poller_destroy(poller *poller) {
  free(poller->nodes);
  __poller_destroy(poller);
}

//*开启poller线程池,one thread per poller
int poller_start(poller *poller) {
  pthread_t tid;
  int ret = 0;
  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  if (__poller_open_pipe(poller) >= 0) {
    ret = pthread_create(&tid, NULL, __poller_thread_routine, poller);
    if (ret == 0) {
      poller->tid = tid;
      poller->stopped = 0;
    } else {
      errno = ret;
      close(poller->pipe_wr);
      close(poller->pipe_rd);
    }
  }
  // pthread_mutex_unlock(&poller->mutex);
  return -poller->stopped;
}

//*将节点插入到poller
static void __poller_insert_node(struct __poller_node *node, poller *poller) {
  struct __poller_node *end;
  //*指向超时时间最大的节点
  end = list_entry(poller->timeo_list->prev, struct __poller_node, list);
  //*超时链表为空
  if (list_empty(poller->timeo_list)) {
    //*直接添加到超时链表
    list_add_tail(poller->timeo_list->prev, &node->list);
    end = rb_entry(poller->tree_first, struct __poller_node, rb);
  } else if (__timeout_cmp(node, end) >= 0) {
    //*node的超时时间大于超时链表的最大超时时间的节点
    list_add_tail(poller->timeo_list->prev, &node->list);
    return;
  } else {
    //*插入入到红黑树
    __poller_tree_insert(node, poller);
    if (&node->rb != poller->tree_first)
      return;
    end = list_entry(poller->timeo_list->next, struct __poller_node, list);
  }

  //*将节点插入到红黑树中
  if (!poller->tree_first || __timeout_cmp(node, end) < 0)
    __poller_set_timerfd(poller->timerfd, &node->timeout);
}

static void __poller_node_set_timeout(int timeout, struct __poller_node *node) {
  clock_gettime(CLOCK_MONOTONIC, &node->timeout);
  node->timeout.tv_sec += timeout / 1000;
  node->timeout.tv_nsec += timeout % 1000 * 1000000;
  if (node->timeout.tv_nsec >= 1000000000) {
    node->timeout.tv_nsec -= 1000000000;
    node->timeout.tv_sec++;
  }
}

static int __poller_data_get_event(int *event, const struct poller_data *data) {
  switch (data->operation) {
  case PD_OP_READ:
    *event = EPOLLIN | EPOLLET;
    return !!data->message;
  case PD_OP_WRITE:
    *event = EPOLLOUT | EPOLLET;
    return 0;
  case PD_OP_LISTEN:
    *event = EPOLLIN;
    return 1;
  case PD_OP_CONNECT:
    *event = EPOLLOUT | EPOLLET;
    return 0;
  case PD_OP_RECVFROM:
    *event = EPOLLIN | EPOLLET;
    return 1;
  case PD_OP_EVENT:
    *event = EPOLLIN | EPOLLET;
    return 1;
  case PD_OP_NOTIFY:
    *event = EPOLLIN | EPOLLET;
    return 1;
  default:
    errno = EINVAL;
    return -1;
  }
}

static struct __poller_node *__poller_new_node(const struct poller_data *data,
                                               int timeout, poller *poller) {
  struct __poller_node *res = NULL;
  struct __poller_node *node;
  int need_res;
  int event;
  //*超出配置的最大文件描述符限制
  if ((size_t)data->fd >= poller->max_open_files) {
    errno = data->fd < 0 ? EBADF : EMFILE;
    return NULL;
  }

  need_res = __poller_data_get_event(&event, data);
  if (need_res < 0)
    return NULL;

  if (need_res) {
    res = (struct __poller_node *)malloc(sizeof(struct __poller_node));
    if (!res)
      return NULL;
  }

  node = (struct __poller_node *)malloc(sizeof(struct __poller_node));
  if (node) {
    node->data = *data;
    node->event = event;
    node->in_rbtree = 0;
    node->removed = 0;
    node->res = res;
    //*如果当前节点存在超时时间
    if (timeout >= 0)
      __poller_node_set_timeout(timeout, node);
  }

  return node;
}

int poller_add(const struct poller_data *data, int timeout, poller *poller) {
  struct __poller_node *node;
  node = __poller_new_node(data, timeout, poller);
  if (!node)
    return -1;

  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  if (!poller->nodes[data->fd]) {
    if (__poller_add_fd(data->fd, node->event, node, poller) >= 0) {
      if (timeout >= 0)
        __poller_insert_node(node, poller);
      else
        list_add_tail(poller->no_timeo_list->prev, &node->list);

      poller->nodes[data->fd] = node;
      node = NULL;
    }
  } else
    errno = EEXIST; //*要添加的文件描述符已存在

  // pthread_mutex_unlock(&poller->mutex);
  if (node == NULL)
    return 0;

  free(node->res);
  free(node);
  return -1;
}

int poller_del(int fd, poller *poller) {
  struct __poller_node *node;
  int stopped = 0;

  if ((size_t)fd >= poller->max_open_files) {
    errno = fd < 0 ? EBADF : EMFILE;
    return -1;
  }

  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  node = poller->nodes[fd];
  if (node) {
    poller->nodes[fd] = NULL;

    if (node->in_rbtree)
      __poller_tree_erase(node, poller);
    else
      list_delete(&node->list);

    __poller_delete_fd(fd, node->event, poller);

    node->error = 0;
    node->state = PR_ST_DELETED;
    stopped = poller->stopped;
    if (!stopped) {
      node->removed = 1;
      write(poller->pipe_wr, &node, sizeof(void *));
    }
  } else
    errno = ENOENT;

  // pthread_mutex_unlock(&poller->mutex);
  if (stopped) {
    free(node->res);
    poller->callback((struct poller_result *)node, poller->context);
  }
  //*先对 node 取逻辑非（!）操作，然后再对结果取负（-）
  return -!node;
}

int poller_mod(const struct poller_data *data, int timeout, poller *poller) {
  struct __poller_node *node;
  struct __poller_node *orig;
  int stopped = 0;

  node = __poller_new_node(data, timeout, poller);
  if (!node)
    return -1;

  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  orig = poller->nodes[data->fd];
  if (orig) {
    if (__poller_modify_fd(data->fd, orig->event, node->event, node, poller) >=
        0) {
      if (orig->in_rbtree)
        __poller_tree_erase(orig, poller);
      else
        list_delete(&orig->list);

      orig->error = 0;
      orig->state = PR_ST_MODIFIED;
      stopped = poller->stopped;
      if (!stopped) {
        orig->removed = 1;
        write(poller->pipe_wr, &orig, sizeof(void *));
      }

      if (timeout >= 0)
        __poller_insert_node(node, poller);
      else
        list_add_tail(poller->no_timeo_list->prev, &node->list);

      poller->nodes[data->fd] = node;
      node = NULL;
    }
  } else
    errno = ENOENT;

  // pthread_mutex_unlock(&poller->mutex);
  if (stopped) {
    free(orig->res);
    poller->callback((struct poller_result *)orig, poller->context);
  }

  if (node == NULL)
    return 0;

  free(node->res);
  free(node);
  return -1;
}

int poller_set_timeout(int fd, int timeout, poller *poller) {
  struct __poller_node time_node;
  struct __poller_node *node;

  if ((size_t)fd >= poller->max_open_files) {
    errno = fd < 0 ? EBADF : EMFILE;
    return -1;
  }

  if (timeout >= 0)
    __poller_node_set_timeout(timeout, &time_node);

  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  node = poller->nodes[fd];
  if (node) {
    if (node->in_rbtree)
      __poller_tree_erase(node, poller);
    else
      list_delete(&node->list);

    if (timeout >= 0) {
      node->timeout = time_node.timeout;
      __poller_insert_node(node, poller);
    } else
      list_add_tail(poller->no_timeo_list->prev, &node->list);
  } else
    errno = ENOENT;

  // pthread_mutex_unlock(&poller->mutex);
  return -!node;
}

int poller_add_timer(const struct timespec *value, void *context, void **timer,
                     poller *poller) {
  struct __poller_node *node;

  node = (struct __poller_node *)malloc(sizeof(struct __poller_node));
  if (node) {
    memset(&node->data, 0, sizeof(struct poller_data));
    node->data.operation = PD_OP_TIMER;
    node->data.fd = -1;
    node->data.context = context;
    node->in_rbtree = 0;
    node->removed = 0;
    node->res = NULL;

    clock_gettime(CLOCK_MONOTONIC, &node->timeout);
    node->timeout.tv_sec += value->tv_sec;
    node->timeout.tv_nsec += value->tv_nsec;
    if (node->timeout.tv_nsec >= 1000000000) {
      node->timeout.tv_nsec -= 1000000000;
      node->timeout.tv_sec++;
    }

    *timer = node;
    // pthread_mutex_lock(&poller->mutex);
    std::unique_lock<std::mutex> lock(poller->mutex);
    __poller_insert_node(node, poller);
    // pthread_mutex_unlock(&poller->mutex);
    return 0;
  }

  return -1;
}

int poller_del_timer(void *timer, poller *poller) {
  struct __poller_node *node = (struct __poller_node *)timer;

  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  if (!node->removed) {
    node->removed = 1;

    if (node->in_rbtree)
      __poller_tree_erase(node, poller);
    else
      list_delete(&node->list);
  } else {
    errno = ENOENT;
    node = NULL;
  }

  // pthread_mutex_unlock(&poller->mutex);
  if (node) {
    node->error = 0;
    node->state = PR_ST_DELETED;
    poller->callback((struct poller_result *)node, poller->context);
    return 0;
  }

  return -1;
}

void poller_stop(poller *poller) {
  struct __poller_node *node;
  // struct list *pos, *tmp;
  struct list node_list;
  list_init(&node_list);
  void *p = NULL;

  write(poller->pipe_wr, &p, sizeof(void *));
  pthread_join(poller->tid, NULL);
  poller->stopped = 1;

  // pthread_mutex_lock(&poller->mutex);
  std::unique_lock<std::mutex> lock(poller->mutex);
  close(poller->pipe_wr);
  __poller_handle_pipe(poller);
  close(poller->pipe_rd);

  poller->tree_first = NULL;
  poller->tree_last = NULL;
  while (poller->timeo_tree.rb_node) {
    node = rb_entry(poller->timeo_tree.rb_node, struct __poller_node, rb);
    rb_erase(&node->rb, &poller->timeo_tree);
    list_add_tail(&node_list, &node->list);
  }

  list_add_list(&node_list, poller->timeo_list);
  list_add_list(&node_list, poller->no_timeo_list);
  list *temp = node_list.next;
  while (temp != &node_list) {
    node = list_entry(temp, struct __poller_node, list);
    if (node->data.fd >= 0) {
      poller->nodes[node->data.fd] = NULL;
      __poller_delete_fd(node->data.fd, node->event, poller);
    } else {
      node->removed = 1;
    }
    temp = temp->next;
  }

  lock.unlock();
  // list_for_each_safe(pos, tmp, &node_list) {
  //   node = list_entry(pos, struct __poller_node, list);
  //   node->error = 0;
  //   node->state = PR_ST_STOPPED;
  //   free(node->res);
  //   poller->callback((struct poller_result *)node, poller->context);
  // }
  temp = node_list.next;
  while (temp != &node_list) {
    node = list_entry(temp, struct __poller_node, list);
    node->error = 0;
    node->state = PR_ST_STOPPED;
    free(node->res);
    poller->callback((struct poller_result *)node, poller->context);
    temp = temp->next;
  }
}
