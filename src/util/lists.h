/************************************************************//**
*
*	@file: lists.h
*	@author: Martin Fouilleul
*	@date: 22/11/2017
*	@revision: 28/04/2019 : deleted containers which are not used by BLITz
*	@brief: Implements generic intrusive linked list and dynamic array
*
****************************************************************/
#ifndef __CONTAINERS_H_
#define __CONTAINERS_H_

#include"debug_log.h"

#ifdef __cplusplus
extern "C" {
#endif

//-------------------------------------------------------------------------
// Intrusive linked lists
//-------------------------------------------------------------------------

#define OFFSET_OF_CONTAINER(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

#ifdef __cplusplus
#define CONTAINER_OF(ptr, type, member) ({          \
		    const decltype( ((type *)0)->member ) *__mptr = (ptr);    \
		    (type *)( (char *)__mptr - OFFSET_OF_CONTAINER(type,member) );})
#else
#define CONTAINER_OF(ptr, type, member) ({          \
		    const char *__mptr = (char*)(ptr);    \
		    (type *)(__mptr - OFFSET_OF_CONTAINER(type,member) );})
#endif

#define ListEntry(ptr, type, member) \
	CONTAINER_OF(ptr, type, member)

#define ListNextEntry(head, elt, type, member) \
	((elt->member.next != ListEnd(head)) ? ListEntry(elt->member.next, type, member) : 0)

#define ListPrevEntry(head, elt, type, member) \
	((elt->member.prev != ListEnd(head)) ? ListEntry(elt->member.prev, type, member) : 0)

#define ListCheckedEntry(head, info, type, member) \
	((info != ListEnd(head)) ? ListEntry(info, type, member) : 0)

#define ListFirstEntry(head, type, member) \
	(ListCheckedEntry(head, ListBegin(head), type, member))

#define ListLastEntry(head, type, member) \
	(ListCheckedEntry(head, ListLast(head), type, member))

#define for_each_in_list(list, elt, type, member)			\
	for(type* elt = ListEntry(ListBegin(list), type, member);	\
	    &elt->member != ListEnd(list);				\
	    elt = ListEntry(elt->member.next, type, member))		\


#define for_each_in_list_reverse(list, elt, type, member)		\
	for(type* elt = ListEntry(ListLast(list), type, member);	\
	    &elt->member != ListEnd(list);				\
	    elt = ListEntry(elt->member.prev, type, member))		\


#define for_each_in_list_safe(list, elt, type, member)			\
	for(type* elt = ListEntry(ListBegin(list), type, member),	\
	    *__tmp = ListEntry(elt->member.next, type, member);		\
	    &elt->member != ListEnd(list);				\
	    elt = ListEntry(&__tmp->member, type, member),		\
	    __tmp = ListEntry(elt->member.next, type, member))		\


#define ListPush(a, b) ListInsert(a, b)
#define ListInsertBefore(a, b) ListAppend(a, b)

#define ListPopEntry(list, type, member) (ListEmpty(list) ? 0 : ListEntry(ListPop(list), type, member))

typedef struct list_info list_info;
struct list_info
{
	list_info* next;
	list_info* prev;
};

static inline void ListInit(list_info* info)
{
	info->next = info->prev = info;
}

static inline list_info* ListBegin(list_info* head)
{
	return(head->next ? head->next : head );
}
static inline list_info* ListEnd(list_info* head)
{
	return(head);
}

static inline list_info* ListLast(list_info* head)
{
	return(head->prev ? head->prev : head);
}

static inline void ListInsert(list_info* head, list_info* elt)
{
	elt->prev = head;
	elt->next = head->next;
	if(head->next)
	{
		head->next->prev = elt;
	}
	else
	{
		head->prev = elt;
	}
	head->next = elt;

	ASSERT(elt->next != elt, "ListInsert(): can't insert an element into itself");
}

static inline void ListAppend(list_info* head, list_info* elt)
{
	ListInsert(head->prev, elt);
}

static inline void ListCat(list_info* head, list_info* list)
{
	if(head->prev)
	{
		head->prev->next = list->next;
	}
	if(head->prev && head->prev->next)
	{
		head->prev->next->prev = head->prev;
	}
	head->prev = list->prev;
	if(head->prev)
	{
		head->prev->next = head;
	}
	ListInit(list);
}

static inline void ListRemove(list_info* elt)
{
	if(elt->prev)
	{
		elt->prev->next = elt->next;
	}
	if(elt->next)
	{
		elt->next->prev = elt->prev;
	}
	elt->prev = elt->next = 0;
}

static inline list_info* ListPop(list_info* head)
{
	list_info* it = ListBegin(head);
	if(it != ListEnd(head))
	{
		ListRemove(it);
		return(it);
	}
	else
	{
		return(0);
	}

}

static inline list_info* ListPopBack(list_info* head)
{
	list_info* it = ListLast(head);
	if(it != ListEnd(head))
	{
		ListRemove(it);
		return(it);
	}
	else
	{
		return(0);
	}
}

static inline bool ListEmpty(list_info* head)
{
	return(head->next == 0 || head->next == head);
}

/*
NOTE(martin): operations with explicit handle

	This variant of the intrusive list can be used when a struct containing a list_info handle can be moved in memory
	(eg. when storing it in a dynamic array or a std::vector). In these scenarios we can not assume that the first
	element of the list will always point back to the handle. Hence we need to explicitly tell where the handle is for
	the list operations to update it :

	- an "explicit" handle points to the first and last element of the list
	- the first element of the list has its prev pointer equal to 0
	- the last element of the list has its next pointer equal to 0
	- the handle can be updated by the Insert/Append/Remove operations


	Note that elements that are linked INSIDE the list are still required not to move in order to preserve the links.
*/

typedef list_info list_handle;	// we use this typedef as a declaration of intent for explicit handles

static inline void ListExplicitInit(list_handle* handle)
{
	handle->next = handle->prev = 0;
}

static inline void ListExplicitInsert(list_handle* list, list_info* after, list_info* elt)
{
	elt->prev = (after == list) ? 0 : after->prev;
	elt->next = after->next;

	if(after->next)
	{
		after->next->prev = elt;
	}
	else
	{
		list->prev = elt;
	}
	after->next = elt;

	ASSERT((elt->next != elt), "ListInsertExplicit(): can't insert an element into itself");
}

static inline void ListExplicitAppend(list_handle* list, list_info* elt)
{
	ListExplicitInsert(list, list->prev ? list->prev : list, elt);
}

static inline void ListExplicitRemove(list_handle* list, list_info* elt)
{
	if(elt->prev)
	{
		elt->prev->next = elt->next;
	}
	else
	{
		list->next = elt->next;
	}

	if(elt->next)
	{
		elt->next->prev = elt->prev;
	}
	else
	{
		list->prev = elt->prev;
	}
	elt->prev = elt->next = 0;
}

#define for_each_in_explicit_list(list, elt, type, member)		\
	for(type* elt = ListEntry(ListBegin(list), type, member);	\
	    elt && (&elt->member != ListEnd(list));			\
	    elt = elt->member.next ? ListEntry(elt->member.next, type, member) : 0)	\

#ifdef __cplusplus
} // extern "C"
#endif

#endif //__CONTAINERS_H_
