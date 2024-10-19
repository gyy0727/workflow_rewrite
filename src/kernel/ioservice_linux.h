#ifndef _IOSERVICE_LINUX_H_
#define _IOSERVICE_LINUX_H_

#include "list.h"
#include <mutex>
#include <pthread.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <sys/uio.h>

#define IOS_STATE_SUCCESS 0
#define IOS_STATE_ERROR 1

//*一次异步IO的交互
class IOSession {
private:
  virtual int prepare() = 0;
  virtual void handle(int state, int error) = 0;

protected:
  //*读
  void prep_pread(int fd, void *buf, size_t count, long long offset);
  //*写
  void prep_pwrite(int fd, void *buf, size_t count, long long offset);
  //*读v
  void prep_preadv(int fd, const struct iovec *iov, int iovcnt,
                   long long offset);
  //*写v
  void prep_pwritev(int fd, const struct iovec *iov, int iovcnt,
                    long long offset);
  //*将一个文件的所有更改强制写入磁盘
  void prep_fsync(int fd);
  //*只保证文件的数据被同步到磁盘
  void prep_fdsync(int fd);

protected:
  //*结果
  long get_res() const { return this->res; }

private:
  char iocb_buf[64];
  long res;

private:
  struct list m_list;

public:
  virtual ~IOSession() {}
  friend class IOService;
  friend class Communicator;
};

class IOService {
public:
  int init(int maxevents);
  void deinit();
  //*执行IOSession
  int request(IOSession *session);

private:
  virtual void handle_stop(int error) {}
  virtual void handle_unbound() = 0;

private:
  //*用于提交到epoll中监听异步IO的完成
  virtual int create_event_fd() { return eventfd(0, 0); }

private:
  //*异步IO上下文
  struct io_context *io_ctx;

private:
  void incref();
  void decref();

private:
  int event_fd; //*事件文件描述符
  int ref;      //*引用计数

private:
  struct list m_session_list;
  std::mutex m_mutex;

private:
  //* 异步IO完成的回调
  static void *aio_finish(void *context);

public:
  virtual ~IOService() {}
  friend class Communicator;
};

#endif
