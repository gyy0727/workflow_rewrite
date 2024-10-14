#include "thrdpool.h"
#include "msgqueue.h"
#include <condition_variable>
#include <cstdlib>
#include <errno.h>
#include <mutex>
#include <pthread.h>
#include <stdlib.h>
static thread_local thrdpool *parent_thrdpool; //*每个线程所属的线程池
struct thrdpool {
  msgqueue *msgqueue;                //*任务队列
  size_t nthreads;                   //*线程数
  size_t stacksize;                  //*线程栈大小
  pthread_t tid;                     //*线程池id
  std::mutex mutex;                  //*互斥锁
  std::condition_variable terminate; //*线程池终止信号
  bool stop;
};

struct __thrdpool_task_entry {
  void *link;                //*next指针
  struct thrdpool_task task; //*线程任务
};

static pthread_t __zero_tid;

//*线程终止函数
//*context为thrdpool类型
static void __thrdpool_exit_routine(void *context) {
  thrdpool *pool = (thrdpool *)context;
  pthread_t tid;
  std::unique_lock<std::mutex> lock(pool->mutex);
  tid = pool->tid;
  pool->tid = pthread_self();
  //*当前线程是最后一个任务线程
  if (--pool->nthreads == 0 && pool->stop) {
    pool->terminate.notify_one();
  }
  lock.unlock();
  if (!pthread_equal(tid, __zero_tid))
    pthread_join(tid, NULL);
  //*退出当前线程
  pthread_exit(NULL);
}

//*每个线程的执行函数
static void *__thrdpool_routine(void *context) {
  thrdpool *pool = (thrdpool *)context;
  struct __thrdpool_task_entry *entry;
  void (*task_routine)(void *); //*任务函数
  void *task_context;           //*执行任务函数需要的上下文

  parent_thrdpool = pool;
  while (!pool->stop) {
    entry = (struct __thrdpool_task_entry *)msgqueue_get(pool->msgqueue);
    if (!entry) { //*任务队列为空
      break;
    }
    task_routine = entry->task.routine;
    task_context = entry->task.context;
    free(entry);
    task_routine(task_context); //*执行任务函数
    if (pool->nthreads == 0) {  //*任务线程被设置为0
      free(pool);
      return nullptr;
    }
  }
  __thrdpool_exit_routine(pool);
  return nullptr;
}

static void __thrdpool_terminate(int in_pool, thrdpool *pool) {
  // pthread_cond_t term = PTHREAD_COND_INITIALIZER;
  std::unique_lock<std::mutex> lock(pool->mutex);
  msgqueue_set_nonblock(pool->msgqueue);
  // pool->terminate = term;
  pool->stop = true;
  if (in_pool) {
    pthread_detach(pthread_self());
    pool->nthreads--;
  }
  while (pool->nthreads > 0) {
    pool->terminate.wait(lock);
  }
  lock.unlock();
  if (!pthread_equal(pool->tid, __zero_tid))
    pthread_join(pool->tid, NULL);
}

static int __thrdpool_create_threads(size_t nthreads, thrdpool *pool) {
  pthread_attr_t attr;
  pthread_t tid;
  int ret;
}

thrdpool *thrdpool_create(size_t nthreads, size_t stacksize) {}

inline void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
                                thrdpool *pool);

void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
                         thrdpool *pool) {}

int thrdpool_schedule(const struct thrdpool_task *task, thrdpool *pool) {}

inline int thrdpool_in_pool(thrdpool *pool);

int thrdpool_in_pool(thrdpool *pool) {}

int thrdpool_increase(thrdpool *pool) {}

int thrdpool_decrease(thrdpool *pool) {}

void thrdpool_exit(thrdpool *pool) {}

void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
                      thrdpool *pool) {}
