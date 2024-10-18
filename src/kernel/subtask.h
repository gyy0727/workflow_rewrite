#ifndef _SUBTASK_H_
#define _SUBTASK_H_

#include <stddef.h>

class ParallelTask; //*并行任务

//*所有任务的base类
class SubTask {
public:
  //*执行subtask
  virtual void dispatch() = 0;

private:
  //*subtask执行完毕,返回下一个subtask
  virtual SubTask *done() = 0;

protected:
  void subtask_done();

public:
  void *get_pointer() const { return this->pointer; }
  void set_pointer(void *pointer) { this->pointer = pointer; }

private:
  ParallelTask *parent; //*属于哪个并行任务
  void *pointer;        //*属于哪个串行任务

public:
  SubTask() {
    this->parent = NULL;
    this->pointer = NULL;
  }

  virtual ~SubTask() {}
  friend class ParallelTask;
};

class ParallelTask : public SubTask {
public:
  virtual void dispatch();

protected:
  SubTask **subtasks; //*子任务
  size_t subtasks_nr; //*子任务数量

private:
  size_t nleft;

public:
  ParallelTask(SubTask **subtasks, size_t n) {
    this->subtasks = subtasks;
    this->subtasks_nr = n;
  }

  virtual ~ParallelTask() {}
  friend class SubTask;
};

#endif