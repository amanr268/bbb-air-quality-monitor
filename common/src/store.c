#include "aqm/store.h"

#include <sqlite3.h>
#include <stdlib.h>

struct aqm_store {
    sqlite3      *db;
    sqlite3_stmt *ins;
    sqlite3_stmt *prune;
    sqlite3_stmt *count;
};

static const char *SCHEMA =
    /* WAL + NORMAL: far fewer fsyncs (less SD wear); on power loss at most
     * the last committed batch can be lost, never a corrupted database */
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS samples ("
    "  seq        INTEGER,"
    "  t_wall     INTEGER NOT NULL,"      /* unix seconds */
    "  status     INTEGER,"
    "  pm1        INTEGER, pm25 INTEGER, pm10 INTEGER,"
    "  tvoc       INTEGER, eco2 INTEGER,"
    "  aht_t_raw  INTEGER, aht_rh_raw INTEGER,"
    "  uploaded   INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_samples_t  ON samples(t_wall);"
    "CREATE INDEX IF NOT EXISTS idx_samples_up ON samples(uploaded) WHERE uploaded = 0;";

aqm_store_t *aqm_store_open(const char *path)
{
    aqm_store_t *st = calloc(1, sizeof(*st));

    if (!st)
        return NULL;
    if (sqlite3_open(path, &st->db) != SQLITE_OK)
        goto fail;
    sqlite3_busy_timeout(st->db, 1000);
    if (sqlite3_exec(st->db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK)
        goto fail;

    if (sqlite3_prepare_v2(st->db,
            "INSERT INTO samples (seq, t_wall, status, pm1, pm25, pm10, tvoc, eco2,"
            " aht_t_raw, aht_rh_raw) VALUES (?,?,?,?,?,?,?,?,?,?)",
            -1, &st->ins, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(st->db, "DELETE FROM samples WHERE t_wall < ?",
            -1, &st->prune, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(st->db, "SELECT COUNT(*) FROM samples",
            -1, &st->count, NULL) != SQLITE_OK)
        goto fail;

    return st;

fail:
    aqm_store_close(st);
    return NULL;
}

void aqm_store_close(aqm_store_t *st)
{
    if (!st)
        return;
    sqlite3_finalize(st->ins);
    sqlite3_finalize(st->prune);
    sqlite3_finalize(st->count);
    sqlite3_close(st->db);
    free(st);
}

int aqm_store_add_batch(aqm_store_t *st, const aqm_sample_t *s, int n)
{
    int i;

    if (sqlite3_exec(st->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    for (i = 0; i < n; i++) {
        sqlite3_bind_int64(st->ins, 1, s[i].seq);
        sqlite3_bind_int64(st->ins, 2, s[i].t_wall_s);
        sqlite3_bind_int64(st->ins, 3, s[i].status);
        sqlite3_bind_int(st->ins, 4, s[i].pm1);
        sqlite3_bind_int(st->ins, 5, s[i].pm25);
        sqlite3_bind_int(st->ins, 6, s[i].pm10);
        sqlite3_bind_int(st->ins, 7, s[i].tvoc_ppb);
        sqlite3_bind_int(st->ins, 8, s[i].eco2_ppm);
        sqlite3_bind_int64(st->ins, 9, s[i].aht_t_raw);
        sqlite3_bind_int64(st->ins, 10, s[i].aht_rh_raw);

        if (sqlite3_step(st->ins) != SQLITE_DONE) {
            sqlite3_reset(st->ins);
            sqlite3_exec(st->db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        sqlite3_reset(st->ins);
    }

    if (sqlite3_exec(st->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(st->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

int aqm_store_prune(aqm_store_t *st, int64_t older_than_s)
{
    int rc;

    sqlite3_bind_int64(st->prune, 1, older_than_s);
    rc = sqlite3_step(st->prune);
    sqlite3_reset(st->prune);
    return rc == SQLITE_DONE ? sqlite3_changes(st->db) : -1;
}

int64_t aqm_store_count(aqm_store_t *st)
{
    int64_t n = -1;

    if (sqlite3_step(st->count) == SQLITE_ROW)
        n = sqlite3_column_int64(st->count, 0);
    sqlite3_reset(st->count);
    return n;
}

const char *aqm_store_errmsg(aqm_store_t *st)
{
    return (st && st->db) ? sqlite3_errmsg(st->db) : "out of memory";
}
