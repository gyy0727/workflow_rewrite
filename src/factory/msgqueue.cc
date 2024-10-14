
#include "msgqueue.h"
#include <assert.h>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <errno.h>
#include <mutex>
#include <pthread.h>
#include <tuple>
struct __msgqueue {
  size_t msg_max;                   //*最大的消息数量
  size_t msg_cnt;                   //*当前消息数量
  int linkoff;                      //*代表消息的大小
  int nonblock;                     //*是否阻塞
  void *head1;                      //*缓冲区1
  void *head2;                      //*缓冲区2
  void **get_head;                  //*获取头部
  void **put_head;                  //*存放头部
  void **put_tail;                  //*存放尾部
  std::mutex get_mutex;             //*获取锁
  std::mutex put_mutex;             //*存放锁
  std::condition_variable get_cond; //*获取信号量
  std::condition_variable put_cond; //*存放信号量
};

void msgqueue_set_nonblock(msgqueue *queue) {
  queue->nonblock = 1;
  std::unique_lock<std::mutex> lock(queue->put_mutex);
  queue->get_cond.notify_one();
  queue->put_cond.notify_all();
}

void msg_queue_set_block(msgqueue *queue) { queue->nonblock = 0; }

void msg_queue_put(void *msg, msgqueue *queue) {
  void **link = (void **)((char *)msg + queue->linkoff);
  //*先转换成char*,然后加上linkoff个字节
  //*现在指向的是list的next指针
  //*然后char*转换成void**,这个过程先将char*转换才void*
  //*最后将void*转换成void**
  //*((char *)msg + queue->linkoff)算出的是消息中用于
  //*链接下一条消息的next指针的地址
  //*所以要转换成二级指针,因为这个地址解引用才是等于next指针的值
  //*link==next指针的地址
  //**link等于next指针的值
  *link = NULL; //*将next指针置为空,代表是最后一条消息
  std::unique_lock<std::mutex> lock(queue->put_mutex);
  while (queue->msg_cnt == queue->msg_max && !queue->nonblock) {
    queue->put_cond.wait(lock);
  }
  *queue->put_tail = link; //*将当前最后一条消息->next指向msg,
  queue->put_tail = link;  //*将尾指针设置为当前消息的地址
  queue->msg_cnt++;
  lock.unlock();
  queue->get_cond.notify_one(); //*存入一条消息,就唤醒一个线程
}

void msgqueue_put_head(void *msg, msgqueue *queue) {
  void **link =
      (void **)((char *)msg + queue->linkoff); //*同理,先用指针指向next指针
  std::unique_lock<std::mutex> lock(queue->put_mutex);
  while (*queue->get_head) { //*存在消息,不为空
    // std::unique_lock<std::mutex> lock1;
    if (queue->get_mutex.try_lock() == 0) { //*先加锁,不给取消息
      lock.unlock(); //*因为操作的是get_head,所以释放put_mutex
      *link = *queue->get_head; //*将当前消息的next指针指向get_head
      *queue->get_head = link;  //*current = curren->next
      queue->get_mutex.unlock();
      return;
    }
  }
  //*消息数量已达上限且队列为阻塞模式
  while (queue->msg_cnt == queue->msg_max && queue->nonblock)
    queue->put_cond.wait(lock); //*等待其他线程取出消息

  *link = *queue->put_head; //*因为get_head为空,所以将数据存入put_head
  if (*link == NULL) {      //*放缓冲区也空
    queue->put_tail = link; //*所以tail也指向link
  }
  *queue->put_head = link; //*put_head指向link,即msg的next
  queue->msg_cnt++;
  queue->get_cond.notify_one(); //*取消息,唤醒一个线程
}

static size_t __msgqueue_swap(msgqueue *queue) {
  void **get_head = queue->get_head;
  size_t cnt;
  std::unique_lock<std::mutex> lock(queue->put_mutex);
  while (queue->msg_cnt == 0 &&
         !queue->nonblock) //*等待有新消息写入才交换缓冲区
    queue->get_cond.wait(lock);

  cnt = queue->msg_cnt;
  if (cnt > queue->msg_max - 1)   //*消息数量达到上限
    queue->put_cond.notify_all(); //*通知线程缓冲区满了

  //*交换两个缓冲区
  queue->get_head = queue->put_head;
  queue->put_head = get_head;
  queue->put_tail = get_head;
  queue->msg_cnt = 0;
  return cnt;
}

void *msgqueue_get(msgqueue *queue) {
  void *msg;
  std::unique_lock<std::mutex> lock(queue->get_mutex);
  //*取缓冲区不为空或交换缓冲区成功
  if (*queue->get_head || __msgqueue_swap(queue) > 0) {
    msg = (char *)*queue->get_head - queue->linkoff;
    *queue->get_head = *(void **)*queue->get_head;
  } else
    msg = NULL;

  return msg;
}

msgqueue *msgqueue_create(size_t maxlen, int linkoff) {
  msgqueue *queue = (msgqueue *)malloc(sizeof(msgqueue));

  if (!queue)
    return NULL;

  queue->msg_max = maxlen;
  queue->linkoff = linkoff;
  queue->head1 = NULL;
  queue->head2 = NULL;
  queue->get_head = &queue->head1;
  queue->put_head = &queue->head2;
  queue->put_tail = &queue->head2;
  queue->msg_cnt = 0;
  queue->nonblock = 0;
  return queue;
}

void msgqueue_destroy(msgqueue *queue) { free(queue); }
