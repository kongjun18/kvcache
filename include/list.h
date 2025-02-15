#ifndef __LIST_H__
#define __LIST_H__

/*
 * Singly-linked List declarations.
 */
#define SLIST_HEAD(name, type)                                          \
struct name {                                                           \
    struct type *slh_first; /* first element */                         \
}

#define SLIST_HEAD_INITIALIZER(head)                                    \
    { nullptr }

#define SLIST_ENTRY(type)                                               \
struct {                                                                \
    struct type *sle_next;  /* next element */                          \
}

/*
 * Singly-linked List functions.
 */
#define SLIST_EMPTY(head)    ((head)->slh_first == nullptr)

#define SLIST_FIRST(head)    ((head)->slh_first)

#define SLIST_FOREACH(var, head, field)                                 \
    for ((var) = SLIST_FIRST((head));                                   \
        (var);                                                          \
        (var) = SLIST_NEXT((var), field))

#define SLIST_FOREACH_SAFE(var, head, field, tvar)                      \
    for ((var) = SLIST_FIRST((head));                                   \
        (var) && ((tvar) = SLIST_NEXT((var), field), 1);                \
        (var) = (tvar))

#define SLIST_FOREACH_PREVPTR(var, varp, head, field)                   \
    for ((varp) = &SLIST_FIRST((head));                                 \
        ((var) = *(varp)) != nullptr;                                      \
        (varp) = &SLIST_NEXT((var), field))

#define SLIST_INIT(head) do {                                           \
    SLIST_FIRST((head)) = nullptr;                                         \
} while (0)

#define SLIST_INSERT_AFTER(slistelm, elm, field) do {                   \
    SLIST_NEXT((elm), field) = SLIST_NEXT((slistelm), field);           \
    SLIST_NEXT((slistelm), field) = (elm);                              \
} while (0)

#define SLIST_INSERT_HEAD(head, elm, field) do {                        \
    SLIST_NEXT((elm), field) = SLIST_FIRST((head));                     \
    SLIST_FIRST((head)) = (elm);                                        \
} while (0)

#define SLIST_NEXT(elm, field)    ((elm)->field.sle_next)

#define SLIST_REMOVE(head, elm, type, field) do {                       \
    if (SLIST_FIRST((head)) == (elm)) {                                 \
        SLIST_REMOVE_HEAD((head), field);                               \
    } else {                                                            \
        struct type *curelm = SLIST_FIRST((head));                      \
        while (SLIST_NEXT(curelm, field) != (elm)) {                    \
            curelm = SLIST_NEXT(curelm, field);                         \
        }                                                               \
        SLIST_REMOVE_AFTER(curelm, field);                              \
    }                                                                   \
} while (0)

#define SLIST_REMOVE_AFTER(elm, field) do {                             \
    QMD_SAVELINK(oldnext, SLIST_NEXT(SLIST_NEXT(elm, field), field));   \
    SLIST_NEXT(elm, field) = SLIST_NEXT(SLIST_NEXT(elm, field), field); \
    TRASHIT(*oldnext);                                                  \
} while (0)

#define SLIST_REMOVE_HEAD(head, field) do {                             \
    QMD_SAVELINK(oldnext, SLIST_NEXT(SLIST_FIRST((head)), field));      \
    SLIST_FIRST((head)) = SLIST_NEXT(SLIST_FIRST((head)), field);       \
    TRASHIT(*oldnext);                                                  \
} while (0)

#endif

