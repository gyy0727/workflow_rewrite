#include "communicator.h"
#include "list.h"
#include "mpoller.h"
#include "msgqueue.h"
#include "poller.h"
#include "thrdpool.h"
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

//*类似于thrdpool_entry,用于提交一个任务
struct CommConnEntry {
  struct list list_;            //*用于链表
  CommConnection *conn_;        //*连接
  long long seq_;               //*序列号
  int sockfd_;                  //*连接描述符
#define CONN_STATE_CONNECTING 0 //*连接中
#define CONN_STATE_CONNECTED 1  //*已连接
#define CONN_STATE_RECEIVING 2  //*接受中
#define CONN_STATE_SUCCESS 3    //*成功
#define CONN_STATE_IDLE 4       //*空闲
#define CONN_STATE_KEEPALIVE 5  //*保持连接
#define CONN_STATE_CLOSING 6    //*关闭
#define CONN_STATE_ERROR 7      //*错误
  int state_;                   //*状态
  int error_;                   //*错误码
  int ref_;                     //*引用计数
  struct iovec *write_iov_;     //*写结构体
  CommSession *session_;        //*所属会话
  CommTarget *target_;          //*对端信息
  CommService *service_;        //*所属listen服务
  mpoller *mpoller_;            //*所属IO多路复用器
  std::mutex mutex_;
};

//*将文件描述符设置为非阻塞
static inline int __set_fd_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags >= 0)
    flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return flags;
}

//*将IP地址绑定到sockfd上
static int __bind_sockaddr(int sockfd, const struct sockaddr *addr,
                           socklen_t addrlen) {
  struct sockaddr_storage ss;
  socklen_t len;
  len = sizeof(struct sockaddr_storage);
  if (getsockname(sockfd, (struct sockaddr *)&ss, &len) < 0)
    return -1;
  ss.ss_family = 0;
  while (len != 0) {
    if (((char *)&ss)[--len] != 0)
      break;
  }
  if (len == 0) {
    if (bind(sockfd, addr, addrlen) < 0)
      return -1;
  }
  return 0;
}

//*释放连接
static void __release_conn(struct CommConnEntry *entry) {
  delete entry->conn_;
  close(entry->sockfd_);
  free(entry);
}

//*初始化一个连接目标
int CommTarget::init(const struct sockaddr *addr, socklen_t addrlen,
                     int connect_timeout, int response_timeout) {
  int ret = 0;
  this->m_addr = (struct sockaddr *)malloc(addrlen);
  if (this->m_addr) {
    // ret = pthread_mutex_init(&this->mutex, NULL);
    memcpy(this->m_addr, addr, addrlen);
    this->m_addrlen = addrlen;
    this->m_connect_timeout = connect_timeout;
    this->m_response_timeout = response_timeout;
    list_init(&this->m_idle_list);
    return 0;
    free(this->m_addr);
  }
  return -1;
}

//*重置
void CommTarget::deinit() {
  // p?thread_mutex_destroy(&this->mutex);
  free(this->m_addr);
}

//*发送数据
int CommMessageIn::feedback(const void *buf, size_t size) {
  struct CommConnEntry *entry = this->entry;
  const struct sockaddr *addr;
  socklen_t addrlen;
  if (entry->service_) {
    entry->target_->get_addr(&addr, &addrlen);
    return sendto(entry->sockfd_, buf, size, 0, addr, addrlen);
  } else
    return write(entry->sockfd_, buf, size);
}

//*重置
void CommMessageIn::renew() {
  CommSession *session = this->entry->session_;
  session->timeout = -1;
  session->begin_time.tv_sec = -1;
  session->begin_time.tv_nsec = -1;
}

//*初始化监听服务
int CommService::init(const struct sockaddr *bind_addr, socklen_t addrlen,
                      int listen_timeout, int response_timeout) {

  this->bind_addr = (struct sockaddr *)malloc(addrlen);
  if (this->bind_addr) {
    memcpy(this->bind_addr, bind_addr, addrlen);
    this->addrlen = addrlen;
    this->listen_timeout = listen_timeout;
    this->response_timeout = response_timeout;
    list_init(&this->alive_list);
    return 0;
  }
  free(this->bind_addr);
  return -1;
}

//*重置
void CommService::deinit() { free(this->bind_addr); }

int CommService::drain(int max) {
  struct CommConnEntry *entry;
  struct list *pos;
  int errno_bak;
  int cnt = 0;

  errno_bak = errno;
  std::unique_lock<std::mutex> lock(this->m_mutex);
  while (cnt != max && !list_empty(&this->alive_list)) {
    pos = this->alive_list.next;
    entry = list_entry(pos, struct CommConnEntry, list_);
    list_delete(pos);
    cnt++;

    /* Cannot change the sequence of next two lines. */
    mpoller_del(entry->sockfd_, entry->mpoller_);
    entry->state_ = CONN_STATE_CLOSING;
  }

  //   pthread_mutex_unlock(&this->m_mutex);
  lock.unlock();
  errno = errno_bak;
  return cnt;
}
