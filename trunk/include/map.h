#ifndef MAP_H
#define MAP_H

#include "datatype.h"

typedef struct map map_t;
typedef struct map_iter map_iter_t;
typedef struct map_impl map_impl_t;

map_t *  map_alloc   (const map_impl_t *map_impl, const datatype_t *key_type);
uint64_t map_get     (map_t *map, void *key);
uint64_t map_set     (map_t *map, void *key, uint64_t new_val);
uint64_t map_add     (map_t *map, void *key, uint64_t new_val);
uint64_t map_cas     (map_t *map, void *key, uint64_t expected_val, uint64_t new_val);
uint64_t map_replace (map_t *map, void *key, uint64_t new_val);
uint64_t map_remove  (map_t *map, void *key);
uint64_t map_count   (map_t *map);
void     map_print   (map_t *map);
void     map_free    (map_t *map);

map_iter_t * map_iter_begin (map_t *map, void *key);
uint64_t     map_iter_next  (map_iter_t *iter, void **key);
void         map_iter_free  (map_iter_t *iter);

/////////////////////////////////////////////////////////////////////////////////////

#define CAS_EXPECT_DOES_NOT_EXIST ( 0)
#define CAS_EXPECT_EXISTS         (-1)
#define CAS_EXPECT_WHATEVER       (-2)

typedef void *   (*map_alloc_t)  (const datatype_t *);
typedef uint64_t (*map_cas_t)    (map_impl_t *, void *, uint64_t, uint64_t);
typedef uint64_t (*map_get_t)    (map_impl_t *, void *);
typedef uint64_t (*map_remove_t) (map_impl_t *, void *);
typedef uint64_t (*map_count_t)  (map_impl_t *);
typedef void     (*map_print_t)  (map_impl_t *);
typedef void     (*map_free_t)   (map_impl_t *);

typedef map_iter_t * (*map_iter_begin_t) (map_impl_t *, void *);
typedef uint64_t     (*map_iter_next_t)  (map_iter_t *, void **);
typedef void         (*map_iter_free_t)  (map_iter_t *);

struct map_impl {
    map_alloc_t  alloc;
    map_cas_t    cas;
    map_get_t    get;
    map_remove_t remove;
    map_count_t  count;
    map_print_t  print;
    map_free_t   free_;

    map_iter_begin_t iter_begin;
    map_iter_next_t  iter_next;
    map_iter_free_t  iter_free;
};

#endif//MAP_H