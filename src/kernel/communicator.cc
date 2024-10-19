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
void CommTarget::deinit() { free(this->m_addr); }

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

//*关闭max个连接
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
    mpoller_del(entry->sockfd_, entry->mpoller_);
    entry->state_ = CONN_STATE_CLOSING;
  }

  lock.unlock();
  errno = errno_bak;
  return cnt;
}

//*增加引用计数
inline void CommService::incref() { __sync_add_and_fetch(&this->ref, 1); }

//*减少引用计数
inline void CommService::decref() {
  if (__sync_sub_and_fetch(&this->ref, 1) == 0)
    this->handle_unbound();
}

//*Target实现
class CommServiceTarget : public CommTarget {
public:
  void incref() { __sync_add_and_fetch(&this->ref, 1); }

  void decref() {
    if (__sync_sub_and_fetch(&this->ref, 1) == 0) {
      this->service->decref();
      this->deinit();
      delete this;
    }
  }

public:
  int shutdown();

private:
  int sockfd; //*文件描述符
  int ref;    //*引用计数

private:
  CommService *service; //*所属的监听服务

private:
  virtual int create_connect_fd() {
    errno = EPERM;
    return -1;
  }
  friend class Communicator;
};

//*关闭连接
int CommServiceTarget::shutdown() {
  struct CommConnEntry *entry;
  int errno_bak;
  int ret = 0;
  std::unique_lock<std::mutex> lock(this->m_mutex);
  if (!list_empty(&this->m_idle_list)) {
    entry = list_entry(this->m_idle_list.next, struct CommConnEntry, list_);
    list_delete(&entry->list_);
    //*如果是tcp
    if (this->service->reliable) {
      errno_bak = errno;
      mpoller_del(entry->sockfd_, entry->mpoller_);
      entry->state_ = CONN_STATE_CLOSING;
      errno = errno_bak;
    } else {
      __release_conn(entry);
      this->decref();
    }
    ret = 1;
  }
  return ret;
}

CommSession::~CommSession() {
  CommServiceTarget *target;
  //*被动
  if (!this->passive)
    return;
  target = (CommServiceTarget *)this->target;
  //*主动,tcp连接
  if (this->passive == 2)
    target->shutdown();
  target->decref();
}

inline int Communicator::first_timeout(CommSession *session) {
  int timeout = session->target->m_response_timeout;

  if (timeout < 0 || (unsigned int)session->timeout <= (unsigned int)timeout) {
    timeout = session->timeout;
    session->timeout = 0;
  } else
    clock_gettime(CLOCK_MONOTONIC, &session->begin_time);

  return timeout;
}

int Communicator::next_timeout(CommSession *session) {
  int timeout = session->target->m_response_timeout;
  struct timespec cur_time;
  int time_used, time_left;
  if (session->timeout > 0) {
    clock_gettime(CLOCK_MONOTONIC, &cur_time);
    time_used = 1000 * (cur_time.tv_sec - session->begin_time.tv_sec) +
                (cur_time.tv_nsec - session->begin_time.tv_nsec) / 1000000;
    time_left = session->timeout - time_used;
    if (time_left <= timeout) /* here timeout >= 0 */
    {
      timeout = time_left < 0 ? 0 : time_left;
      session->timeout = 0;
    }
  }
  return timeout;
}

int Communicator::first_timeout_send(CommSession *session) {
  session->timeout = session->send_timeout();
  return Communicator::first_timeout(session);
}

int Communicator::first_timeout_recv(CommSession *session) {
  session->timeout = session->receive_timeout();
  return Communicator::first_timeout(session);
}

//*停止listen服务
void Communicator::shutdown_service(CommService *service) {
  close(service->listen_fd);
  service->listen_fd = -1;
  service->drain(-1);
  service->decref();
}

#ifndef IOV_MAX
#ifdef UIO_MAXIOV
#define IOV_MAX UIO_MAXIOV
#else
#define IOV_MAX 1024
#endif
#endif