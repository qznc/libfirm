/* Declarations for pset.
   Copyright (C) 1995, 1996 Markus Armbruster */

#ifndef _PSET_H
#define _PSET_H

#include <stddef.h>

typedef struct pset pset;

typedef struct {
  unsigned hash;
  void *dptr;
} pset_entry;


typedef int (*pset_cmp_fun) (const void *, const void *);

pset *new_pset (pset_cmp_fun, int slots);
void del_pset (pset *);

void *pset_find (pset *, const void *key, unsigned hash);
void *pset_insert (pset *, const void *key, unsigned hash);
pset_entry *pset_hinsert (pset *, const void *key, unsigned hash);
void *pset_remove (pset *, const void *key, unsigned hash);

void *pset_first (pset *);
void *pset_next (pset *);
void pset_break (pset *);

#define new_pset(cmp, slots) (PSET_TRACE (new_pset) ((cmp), (slots)))
#define pset_find(pset, key, hash) \
  _pset_search ((pset), (key), (hash), _pset_find)
#define pset_insert(pset, key, hash) \
  _pset_search ((pset), (key), (hash), _pset_insert)
#define pset_hinsert(pset, key, hash) \
  ((pset_entry *)_pset_search ((pset), (key), (hash), _pset_hinsert))

#ifdef STATS
void pset_stats (pset *);
#else
# define pset_stats(s) ((void)0)
#endif

#ifdef DEBUG
void pset_describe (pset *);
#endif

/* @@@ NYI */
#define PSET_VRFY(pset) (void)0


/* Private */

typedef enum { _pset_find, _pset_insert, _pset_hinsert } _pset_action;

void *_pset_search (pset *, const void *, unsigned, _pset_action);

#if defined(DEBUG) && defined(HAVE_GNU_MALLOC)
extern const char *pset_tag;
# ifdef PSET_ID
#   define PSET_TRACE pset_tag = SET_ID,
# else
#   define PSET_TRACE pset_tag = __FILE__,
# endif
#else /* !(DEBUG && HAVE_GNU_MALLOC) */
#   define PSET_TRACE
#endif /* !(DEBUG && HAVE_GNU_MALLOC) */

#endif
