#ifndef _COMMUNICATOR_H_
#define _COMMUNICATOR_H_

#include "list.h"
#include "poller.h"
#include <mutex>
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>

//*所有连接的base类
class CommConnection {
public:
  virtual ~CommConnection() {}
};

//*连接的目标
class CommTarget {
public:
  //*初始化,ip,ip长度,连接超时,响应超时
  int init(const struct sockaddr *addr, socklen_t addrlen, int connect_timeout,
           int response_timeout);
  void deinit();

public:
  //*取出IP地址
  void get_addr(const struct sockaddr **addr, socklen_t *addrlen) const {
    *addr = this->m_addr;
    *addrlen = this->m_addrlen;
  }
  //*return list_empty(m_idle_list)
  int has_idle_conn() const { return !list_empty(&this->m_idle_list); }

private:
  //*创建连接描述符
  virtual int create_connect_fd() {
    return socket(this->m_addr->sa_family, SOCK_STREAM, 0);
  }
  //*创建新连接
  virtual CommConnection *new_connection(int connect_fd) {
    return new CommConnection;
  }

public:
  virtual void release() {}

private:
  struct sockaddr *m_addr; //*对端IP地址
  socklen_t m_addrlen;     //*ip地址长度
  int m_connect_timeout;   //*连接超时
  int m_response_timeout;  //*响应超时

private:
  struct list m_idle_list; //*空闲连接列表
  std::mutex m_mutex;

public:
  virtual ~CommTarget() {}
  friend class CommServiceTarget;
  friend class Communicator;
};

//*发出的消息
class CommMessageOut {
private:
  //*序列化
  virtual int encode(struct iovec vectors[], int max) = 0;

public:
  virtual ~CommMessageOut() {}
  friend class Communicator;
};

class CommMessageIn : private poller_message {
private:
  virtual int append(const void *buf, size_t *size) = 0;

protected:
  virtual int feedback(const void *buf, size_t size);
  virtual void renew();
  virtual CommMessageIn *inner() { return this; }

private:
  struct CommConnEntry *entry; //*对应一次连接实例

public:
  virtual ~CommMessageIn() {}
  friend class Communicator;
};

#define CS_STATE_SUCCESS 0 //*service成功
#define CS_STATE_ERROR 1   //*service失败
#define CS_STATE_STOPPED 2 //*service停止
#define CS_STATE_TOREPLY 3 //*service超时

//*一次交互,request-response
class CommSession {
private:
  virtual CommMessageOut *message_out() = 0;
  virtual CommMessageIn *message_in() = 0;
  virtual int send_timeout() { return -1; }
  virtual int receive_timeout() { return -1; }
  virtual int keep_alive_timeout() { return 0; }
  virtual int first_timeout() { return 0; }
  virtual void handle(int state, int error) = 0;

protected:
  CommTarget *get_target() const { return this->target; }
  CommConnection *get_connection() const { return this->conn; }
  CommMessageOut *get_message_out() const { return this->out; }
  CommMessageIn *get_message_in() const { return this->in; }
  long long get_seq() const { return this->seq; }

private:
  CommTarget *target;   //*会话对应的远端目标
  CommConnection *conn; //*会话对应的连接
  CommMessageOut *out;  //*会话对应的消息发送结构
  CommMessageIn *in;    //*会话对应的消息接收结构
  long long seq;        //*session的序列号

private:
  struct timespec begin_time; //*会话开始时间
  int timeout;                //*超时时间
  int passive; //*是否主动,是主动发起的请求还是响应远端的请求

public:
  CommSession() { this->passive = 0; }
  virtual ~CommSession();
  friend class CommMessageIn;
  friend class Communicator;
};

//*server监听服务
class CommService {
public:
  int init(const struct sockaddr *bind_addr, socklen_t addrlen,
           int listen_timeout, int response_timeout);
  void deinit();
  //*减少连接
  int drain(int max);

public:
  void get_addr(const struct sockaddr **addr, socklen_t *addrlen) const {
    *addr = this->bind_addr;
    *addrlen = this->addrlen;
  }

private:
  virtual CommSession *new_session(long long seq, CommConnection *conn) = 0;
  virtual void handle_stop(int error) {}
  virtual void handle_unbound() = 0;

private:
  virtual int create_listen_fd() {
    return socket(this->bind_addr->sa_family, SOCK_STREAM, 0);
  }

  virtual CommConnection *new_connection(int accept_fd) {
    return new CommConnection;
  }

private:
  struct sockaddr *bind_addr; //*listen的地址
  socklen_t addrlen;
  int listen_timeout;   //*listen操作超时时间
  int response_timeout; //*响应超时

private:
  void incref(); //*引用计数增加
  void decref(); //*引用计数减少

private:
  int reliable;  //*是否可靠,就是有没有监听端口
  int listen_fd; //*监听文件描述符
  int ref;       //*引用计数

private:
  struct list alive_list; //*活跃连接
  std::mutex m_mutex;

public:
  virtual ~CommService() {}
  friend class CommServiceTarget;
  friend class Communicator;
};

//*用于实现sleep
class SleepSession {
private:
  virtual int duration(struct timespec *value) = 0;
  virtual void handle(int state, int error) = 0;

private:
  void *timer; //*定时器
  int index;   //*索引

public:
  virtual ~SleepSession() {}
  friend class Communicator;
};

#endif
