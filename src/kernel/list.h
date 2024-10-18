

#ifndef _LIST_H_
#define _LIST_H_

#include <unistd.h>
struct list {
  list *next;
  list *prev;
};

static inline void list_init(list *head) {
  head->next = head;
  head->prev = head;
}

static inline bool list_empty(const struct list *head) {
  return head->next == head;
}

static inline void list_add_tail(list *head, list *entry) {
  if (list_empty(head)) {
    head->next = entry;
    head->prev = entry;
    entry->prev = head;
    entry->next = head;
    return;
  }
  entry->prev = head->prev->next;
  entry->next = head;
  head->prev->next = entry;
  // entry->next = head;
}
static inline void list_add_list(list *head, list *entry) {
  list *tmp = head->prev;
  tmp->next = entry;
  entry->next = head;
  entry->prev = tmp;
}

static inline void list_add_front(list *head, list *entry) {
  if (list_empty(head)) {
    head->next = entry;
    head->prev = entry;
    entry->prev = head;
    entry->next = head;
    return;
  }
  entry->prev = head;
  entry->next = head->next;
  head->next = entry;
}

static inline void list_delete(list *entry) {
  entry->prev->next = entry->next;
  entry->next->prev = entry->prev;
  // list_init(entry);
}

#define list_entry(ptr, type, member)                                          \
  ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))
#endif
