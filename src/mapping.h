/* mapping.h - 1992/07/19 */

#ifndef _MAPPING_H
#define _MAPPING_H

#define MAP_POINTER_HASH(x) (((POINTER_INT)x) >> 4)

typedef struct mapping_node_s {
    struct mapping_node_s *next;
    svalue_t values[2];
} mapping_node_t;

#define MNB_SIZE 256

typedef struct mapping_node_block_s {
    struct mapping_node_block_s *next;
    mapping_node_t nodes[MNB_SIZE];
} mapping_node_block_t;

#define MAX_TABLE_SIZE 32768
#define MAP_HASH_TABLE_SIZE 8	/* must be a power of 2 */
#define FILL_PERCENT 80		/* must not be larger than 99 */

#define MAPSIZE(size) sizeof(mapping_t)

typedef struct mapping_s {
    unsigned short ref;		/* how many times this map has been
				 * referenced */
#ifdef DEBUG
    int extra_ref;
#endif
    mapping_node_t **table;	/* the hash table */
    unsigned short table_size;	/* # of buckets in hash table == power of 2 */
    unsigned short unfilled;	/* # of buckets among 80% of total buckets that do not have entries */
    int count;			/* total # of nodes actually in mapping  */
#ifdef PACKAGE_MUDLIB_STATS
    statgroup_t stats;		/* creators of the mapping */
#endif
} mapping_t;

typedef struct finfo_s {
    char *func;
    object_t *obj;
    svalue_t *extra;
    funptr_t *fp;
}       finfo_t;

typedef struct vinfo_s {
    array_t *v;
    int pos, size, w;
}       vinfo_t;

typedef struct minfo_s {
    mapping_t *map, *newmap;
}       minfo_t;

#define mapping_too_large() \
    error("Mapping exceeded maximum allowed size of %d.\n",MAX_MAPPING_SIZE);

#ifndef max
#define max(x,y) ((x) > (y)) ? (x) : (y)
#endif

/*
 * mapping.c
 */
extern int num_mappings;
extern int total_mapping_size;
extern int total_mapping_nodes;

int msameval PROT((svalue_t *, svalue_t *));
int mapping_save_size PROT((mapping_t *));
INLINE mapping_t *mapTraverse PROT((mapping_t *, int (*) (mapping_t *, mapping_node_t *, void *), void *));
INLINE mapping_t *load_mapping_from_aggregate PROT((svalue_t *, int));
INLINE mapping_t *allocate_mapping PROT((int));
INLINE void free_mapping PROT((mapping_t *));
INLINE svalue_t *find_in_mapping PROT((mapping_t *, svalue_t *));
svalue_t *find_string_in_mapping PROT((mapping_t *, char *));
INLINE svalue_t *find_for_insert PROT((mapping_t *, svalue_t *, int));
INLINE void absorb_mapping PROT((mapping_t *, mapping_t *));
INLINE void mapping_delete PROT((mapping_t *, svalue_t *));
INLINE mapping_t *add_mapping PROT((mapping_t *, mapping_t *));
mapping_node_t *new_map_node PROT((void));
void free_node PROT((mapping_node_t *));
void map_mapping PROT((svalue_t *, int));
void filter_mapping PROT((svalue_t *, int));
INLINE mapping_t *compose_mapping PROT((mapping_t *, mapping_t *, unsigned short));
array_t *mapping_indices PROT((mapping_t *));
array_t *mapping_values PROT((mapping_t *));
array_t *mapping_each PROT((mapping_t *));
char *save_mapping PROT((mapping_t *));
void dealloc_mapping PROT((mapping_t *));
void mark_mapping_node_blocks PROT((void));

void add_mapping_pair PROT((mapping_t *, char *, int));
void add_mapping_string PROT((mapping_t *, char *, char *));
void add_mapping_malloced_string PROT((mapping_t *, char *, char *));
void add_mapping_object PROT((mapping_t *, char *, object_t *));
void add_mapping_array PROT((mapping_t *, char *, array_t *));
void add_mapping_shared_string PROT((mapping_t *, char *, char *));

#endif				/* _MAPPING_H */
