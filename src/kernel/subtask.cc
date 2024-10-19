#include "subtask.h"

void SubTask::subtask_done() {

  SubTask *cur = this;
  //*获取当前任务所属的并行任务
  ParallelTask *parent;
  while (1) {
    parent = cur->parent;
    cur = cur->done(); //*执行当前任务,并获取下一个任务
    //*有下一个任务
    if (cur) {
      cur->parent = parent;
      cur->dispatch(); //*执行下一个任务
    } else if (parent) {
      //*没有下一个任务但是有父并行任务
      if (__sync_sub_and_fetch(&parent->nleft, 1) == 0) {
        cur = parent; //*接下来会执行父并行任务的dispatch
        continue;
      }
    }

    break;
  }
}

void ParallelTask::dispatch() {
  SubTask **end = this->subtasks + this->subtasks_nr;
  SubTask **p = this->subtasks;

  this->nleft = this->subtasks_nr;
  //*有多个串行任务,将串行任务执行
  if (this->nleft != 0) {
    do {
      (*p)->parent = this;
      (*p)->dispatch();
    } while (++p != end);
  } else
    this->subtask_done(); //*没有串行任务了就subtask_done
}
