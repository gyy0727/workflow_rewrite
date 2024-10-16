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
  struct list timeo_list;       //*已经超时的节点的链表
  struct list no_timeo_list;    //*未超时的节点的链表
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
  int ret;
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
  int ret;

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
      if (nleft >= iov->iov_len) {
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



































