

#ifndef _LIST_H_
#define _LIST_H_

struct list {
  list *next;
  list *prev;
};

static inline void list_init(list *head) {
  head->next = head;
  head->prev = head;
}

static inline void list_add_tail(list *head, list *entry) {
  head->next = entry;
  entry->prev = head;
}

static inline void list_add_front(list *head, list *entry) {
  list *front = head->prev;
  front->next = entry;
  entry->prev = front;
  entry->next = head;
  head->prev = entry;
}

static inline void list_delete(list *entry) {
  entry->prev->next = entry->next;
  entry->next->prev = entry->prev;
  // list_init(entry);
}

static inline int list_empty(const struct list *head) {
  return head->next == head;
}

#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))
#endif

