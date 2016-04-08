#ifndef __LINKEDLIST_H__
#define __LINKEDLIST_H__

struct __listhead
{
	struct __listhead* prev;
	struct __listhead* next;
};

#define LIST_DECLARE(var) \
	struct __listhead var

#define LIST_DECLARE_STATIC(var) \
	static struct __listhead var

#define LIST_HEAD() \
	LIST_DECLARE(__head)

#define LIST_INIT(list) \
	((struct __listhead*)(list))->next = list; \
	((struct __listhead*)(list))->prev = list

#define LIST_EMPTY(list) (((struct __listhead*)(list))->next == (list))

#define LIST_INSERT(iitem, iprev, inext) \
{ \
	((struct __listhead*)(inext))->prev = (struct __listhead*)(iitem); \
	((struct __listhead*)(iitem))->next = (struct __listhead*)(inext); \
	((struct __listhead*)(iitem))->prev = (struct __listhead*)(iprev); \
	((struct __listhead*)(iprev))->next = (struct __listhead*)(iitem); \
}

#define LIST_ADD(list, item) \
	LIST_INSERT(item, list, (list)->next)

#define LIST_APPEND(list, item) \
	LIST_INSERT(item, (list)->prev, list)

#define LIST_REMOVE(item) \
{ \
        struct __listhead *prev = ((struct __listhead*)(item))->prev; \
        struct __listhead *next = ((struct __listhead*)(item))->next; \
        next->prev = prev; \
        prev->next = next; \
}

#define LIST_TAIL(type, list) ((type) ((struct __listhead*)(list))->prev)
#define LIST_NEXT(type, item) ((type) ((struct __listhead*)(item))->next)

#define LIST_FOREACH(type, ivar, list) \
	for (ivar = LIST_NEXT(type, list); ((struct __listhead*) ivar) != list; ivar = LIST_NEXT(type, ivar))

#define LIST_FOREACH_SAFE(type, var, list, codeblock) \
{ \
	struct __listhead* __next; \
	for (var = LIST_NEXT(type, list); ((struct __listhead*) var) != list; var = (type) __next) { \
		__next = (struct __listhead*) LIST_NEXT(type, var); \
		codeblock \
	} \
}

static inline size_t
LIST_SIZE(struct __listhead* list)
{
	size_t sz = 0;
	struct __listhead *ent;
	LIST_FOREACH(struct __listhead*, ent, list) {
		sz++;
	}
	return sz;
}

#endif

