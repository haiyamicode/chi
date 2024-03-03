#ifndef HASHDICTC
#define HASHDICTC
#include <stdint.h> /* uint32_t */
#include <stdlib.h> /* malloc/calloc */
#include <string.h> /* memcpy/memcmp */

#ifdef __x86_64__
#include <xmmintrin.h>
#endif

typedef int (*enumFunc)(void *key, int count, void **value, void *user);

#define HASHDICT_VALUE_TYPE void *
#define KEY_LENGTH_TYPE uint32_t

struct keynode {
    struct keynode *next;
    char *key;
    KEY_LENGTH_TYPE len;
    HASHDICT_VALUE_TYPE value;
};

struct dictionary {
    struct keynode **table;
    int length, count;
    double growth_treshold;
    double growth_factor;
    HASHDICT_VALUE_TYPE *value;
};

/* See README.md */

struct dictionary *dic_new(int initial_size);
void dic_delete(struct dictionary *dic);
int dic_add(struct dictionary *dic, void *key, int keyn);
int dic_find(struct dictionary *dic, void *key, int keyn);
void dic_forEach(struct dictionary *dic, enumFunc f, void *user);
#endif