#ifndef THRDPOOL_H
#define THRDPOOL_H
#include <stddef.h>

struct thrdpool_task {
  void (*routine)(void *);
  void *context;
};
struct thrdpool;
thrdpool *thrdpool_create(size_t nthreads, size_t stacksize);
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool *pool);
int thrdpool_in_pool(thrdpool *pool);
int thrdpool_increase(thrdpool *pool);
int thrdpool_decrease(thrdpool *pool);
void thrdpool_exit(thrdpool *pool);
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
                      thrdpool *pool);

#endif