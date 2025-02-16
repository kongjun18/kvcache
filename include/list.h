#ifndef SIMPLE_LIST_H
#define SIMPLE_LIST_H

#include <stddef.h>

// 定义链表节点结构
struct list_head {
  struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) {&(name), &(name)}

#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
  list->next = list;
  list->prev = list;
}

#define list_first_entry(head, type, member) \
  ((type *)((char *)(head)->next - offsetof(type, member)))

static inline bool list_empty(struct list_head *head) {
  return head->next == head;
}

// Add newh after head
static inline void list_add(struct list_head *newh, struct list_head *head) {
  newh->next = head->next;
  newh->prev = head;
  head->next->prev = newh;
  head->next = newh;
}

static inline void list_add_tail(struct list_head *newh,
                                 struct list_head *head) {
  newh->prev = head->prev;
  newh->next = head;
  head->prev->next = newh;
  head->prev = newh;
}

static inline void list_del(struct list_head *entry) {
  entry->prev->next = entry->next;
  entry->next->prev = entry->prev;
  entry->next = entry->prev = nullptr;
}

// list_for_each doesn't support deletion.
//
// pos  struct list_head * - current position
// head struct list_head * - list head
#define list_for_each(pos, head)                                               \
  for (pos = (head)->next; pos != (head); pos = pos->next)

// It is safe to delete nodes in list_for_each_safe
//
//
// pos  struct list_head * - current position
// next struct list_head * - next position
// head struct list_head * - list head
#define list_for_each_safe(pos, next, head)                                       \
  for (pos = (head)->next, next = pos->next; pos != (head); pos = next, n = pos->next)

#define list_for_each_with_entry(type, entry, head, member)                          \
  for (entry = (type *)((char *)(head)->next -                       \
                                  offsetof(type, member));           \
       &entry->member != (head);                                               \
       entry = (type *)((char *)(entry->member.next) -               \
                                  offsetof(type, member)))

#define list_for_each_safe_with_entry(entry, pos, n, head, member)   \
    for (entry = (typeof(*entry) *)((char *)(head)->next -             \
    offsetof(typeof(*entry), member)), pos = (head)->next,             \
    n = pos->next; pos != (head); pos = n, n = pos->next,              \
    entry = (typeof(*entry) *)((char *)(pos) - offsetof(typeof(*entry), member)))


#endif // SIMPLE_LIST_H
