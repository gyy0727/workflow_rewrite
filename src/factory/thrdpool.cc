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
    if (!entry) { //*任务队列为空,能取出空任务,代表调用了terminate函数里面的set_nonblock
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
  msgqueue_set_nonblock(
      pool->msgqueue); //*防止有线程阻塞在取任务和放入任务,无法感知线程池的终止
  // pool->terminate = term;
  pool->stop = true;
  if (in_pool) {
    pthread_detach(pthread_self());//*在任务线程调用了destroy()
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
  ret = pthread_attr_init(&attr); //*初始化线程创建传入的参数
  if (ret == 0) {
    if (pool->stacksize) {
      pthread_attr_setstacksize(&attr, pool->stacksize);
    }
    while (pool->nthreads < nthreads) {
      ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);
      if (ret == 0) {
        pool->nthreads++;
      } else {
        break;
      }
    }
    pthread_attr_destroy(&attr);
    if (pool->nthreads == nthreads) {
      return 0;
    }
    __thrdpool_terminate(0,
                         pool); //*创建任务线程出现了不符合预期的结果,终止线程池
  }
  errno = ret;
  return -1;
}

//*创建线程池
thrdpool *thrdpool_create(size_t nthreads, size_t stacksize) {
  thrdpool *pool;
  int ret;
  pool = (thrdpool *)malloc(sizeof(thrdpool));
  if (!pool) {
    errno = ret;
    return nullptr;
  }
  pool->msgqueue =
      msgqueue_create(0, 0); //*1:无消息数量上限 2:next指针偏移量为0
  if (pool->msgqueue) {
    pool->stacksize = stacksize;
    pool->nthreads = nthreads;
    pool->tid = __zero_tid; //* 值为0
    pool->stop = false;
    if (__thrdpool_create_threads(nthreads, pool) >= 0) {
      return pool;
    }
  }
  errno = ret;
  msgqueue_destroy(pool->msgqueue);
  free(pool);
  return nullptr;
}

inline void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
                                thrdpool *pool);

void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
                         thrdpool *pool) {
  ((struct __thrdpool_task_entry *)buf)->task = *task;
  msgqueue_put(buf, pool->msgqueue);
}

int thrdpool_schedule(const struct thrdpool_task *task, thrdpool *pool) {
  void *buf = malloc(sizeof(struct __thrdpool_task_entry));
  if (buf) {
    __thrdpool_schedule(task, buf, pool);
    return 0;
  }
  return -1;
}

inline int thrdpool_in_pool(thrdpool *pool);

int thrdpool_in_pool(thrdpool *pool) { return parent_thrdpool == pool; }

int thrdpool_increase(thrdpool *pool) {
  pthread_attr_t attr;
  pthread_t tid;
  int ret;
  ret = pthread_attr_init(&attr);
  if (ret == 0) {
    if (pool->stacksize)
      pthread_attr_setstacksize(&attr, pool->stacksize);
    std::unique_lock<std::mutex> lock(pool->mutex);
    ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);
    if (ret == 0)
      pool->nthreads++;
    pthread_attr_destroy(&attr);
    if (ret == 0)
      return 0;
  }

  errno = ret;
  return -1;
}

int thrdpool_decrease(thrdpool *pool) {
  void *buf = malloc(sizeof(struct __thrdpool_task_entry));
  struct __thrdpool_task_entry *entry;
  if (buf) {
    entry = (struct __thrdpool_task_entry *)buf;
    entry->task.routine = __thrdpool_exit_routine;
    entry->task.context = pool;
    //*获取到__thrdpool_exit_routine任务的函数就会被终止
    msgqueue_put_head(entry, pool->msgqueue);
    return 0;
  }
  return -1;
}

//*当前线程调用__thrdpool_exit_routine函数,用于任务线程的终止,由任务线程调用
void thrdpool_exit(thrdpool *pool) {
  if (thrdpool_in_pool(pool))
    __thrdpool_exit_routine(pool);
}

void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
                      thrdpool *pool) {
  int in_pool = thrdpool_in_pool(pool);
  struct __thrdpool_task_entry *entry;

  __thrdpool_terminate(in_pool, pool);
  while (1) {
    entry = (struct __thrdpool_task_entry *)msgqueue_get(pool->msgqueue);
    if (!entry)
      break;

    if (pending && entry->task.routine != __thrdpool_exit_routine)
      pending(&entry->task);

    free(entry);
  }
  msgqueue_destroy(pool->msgqueue);
  if (!in_pool)
    free(pool);
}
