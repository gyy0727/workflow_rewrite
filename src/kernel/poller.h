#ifndef _POLLER_H_
#define _POLLER_H_

#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

struct poller;
struct poller_message; //*消息,read事件触发的结果

//*handle时使用
struct poller_message {
  int (*append)(const void *, size_t *, poller_message *);
  char data[0];
};

struct poller_data {
#define PD_OP_TIMER 0    //*定时器事件
#define PD_OP_READ 1     //*读事件
#define PD_OP_WRITE 2    //*写事件
#define PD_OP_LISTEN 3   //*监听事件
#define PD_OP_CONNECT 4  //*连接事件
#define PD_OP_RECVFROM 5 //*recvfrom事件
#define PD_OP_EVENT 9    //*linux的event事件
#define PD_OP_NOTIFY 10  //*kqueue的notify事件
  short operation;       //*操作类型
  unsigned short iovcnt; //*iovec的个数
  int fd;                //*文件描述符
  union {
    poller_message *(*create_message)(void *); //*创建消息
    int (*partial_written)(size_t, void *);    //*写入消息的部分
    void *(*accept)(const struct sockaddr *, socklen_t, int,
                    void *); //*接受连接
    void *(*recvfrom)(const struct sockaddr *, socklen_t, const void *, size_t,
                      void *);       //*recvfrom
    void *(*event)(void *);          //*event事件
    void *(*notify)(void *, void *); //*notify事件
  };
  void *context; //*回调需要的上下文
  union {
    poller_message *message;
    struct iovec *write_iov; //*写缓冲区
    void *result;
  };
};

struct poller_result {
#define PR_ST_SUCCESS 0
#define PR_ST_FINISHED 1
#define PR_ST_ERROR 2
#define PR_ST_DELETED 3
#define PR_ST_MODIFIED 4
#define PR_ST_STOPPED 5
  int state;
  int error;
  struct poller_data data;
  /* In callback, spaces of six pointers are available from here. */
};

//*poller创建的参数
struct poller_params {
  size_t max_open_files; //*poller最大打开的文件描述符数量
  void (*callback)(struct poller_result *, void *); //*处理完事件的回调函数
  void *context; //*回调需要的上下文,队列msgqueue
};


poller *poller_create(const struct poller_params *params);
int poller_start(poller *poller);
int poller_add(const struct poller_data *data, int timeout, poller *poller);
int poller_del(int fd, poller *poller);
int poller_mod(const struct poller_data *data, int timeout, poller *poller);
int poller_set_timeout(int fd, int timeout, poller *poller);
int poller_add_timer(const struct timespec *value, void *context, void **timer,
                     poller *poller);
int poller_del_timer(void *timer, poller *poller);
void poller_stop(poller *poller);
void poller_destroy(poller *poller);

#endif
