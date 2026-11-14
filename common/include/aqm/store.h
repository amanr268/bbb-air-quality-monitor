#ifndef AQM_STORE_H
#define AQM_STORE_H

#include <stdint.h>

#include "aqm/types.h"

/* SQLite-backed offline buffer of raw samples.
 * Not thread-safe: use one aqm_store_t from one thread. */
typedef struct aqm_store aqm_store_t;

aqm_store_t *aqm_store_open(const char *path);      /* NULL on error   */
void         aqm_store_close(aqm_store_t *st);

/* insert n samples in ONE transaction; 0 or -1 */
int          aqm_store_add_batch(aqm_store_t *st, const aqm_sample_t *s, int n);
/* delete rows with t_wall < older_than_s; returns rows deleted or -1 */
int          aqm_store_prune(aqm_store_t *st, int64_t older_than_s);
/* total rows, or -1 */
int64_t      aqm_store_count(aqm_store_t *st);

/* call cb for every row with t_wall >= since, oldest first. The sample has
 * the raw fields, status, seq and t_wall_s set (no calibrated values).
 * cb returns non-zero to stop. Returns rows visited or -1. */
int64_t      aqm_store_each_since(aqm_store_t *st, int64_t since,
                                  int (*cb)(const aqm_sample_t *s, void *ctx), void *ctx);

const char  *aqm_store_errmsg(aqm_store_t *st);

#endif
