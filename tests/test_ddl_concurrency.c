// tests/test_ddl_concurrency.c — DDL capture under a concurrent writer.
//
// Field report (1.6.0): an application ran ALTER/CREATE through
// prepare/step while another statement on the same connection was still
// open (a half-read SELECT), and another connection (the flush thread, a
// second process) committed in between. The DDL committed, then the
// trigger resync and the outbox insert both failed with "database is
// locked": the schema change never reached the replica and later rows of
// that table were captured with the old column list. Health stayed green.
//
// The interleaving is made deterministic with a WAL hook: right after the
// application's commit lands, a second connection commits a write. The
// application connection's still-open read is now on a stale snapshot,
// so any later write upgrade on it fails with SQLITE_BUSY immediately (no
// busy-handler retry).
//
// Invariants:
//   1. DDL through prepare/step and through db_exec is captured (outbox
//      row + capture triggers) or fails as a whole; never committed alone.
//   2. Rows written after an ALTER are captured with the new column.
//   3. Preparing DDL through the wrapper does not log the raw-handle
//      "bypassed the backup wrapper" warning.
//   4. Capturing DDL does not change last_insert_rowid seen by the app.

#include "class.h"
#include "deps/sqlite/sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_PATH "ddl_conc.db"

static int n_errors, n_bypass;
static void log_cb(ark_log_level_t level, const char *msg, void *ctx) {
  (void)ctx;
  if (strstr(msg, "bypassed the backup wrapper")) n_bypass++;
  if (level == ARK_LOG_ERROR) {
    n_errors++;
    fprintf(stderr, "    arkilian error: %s\n", msg);
  }
}

static sqlite3 *other;      // a second connection to the same file
static int inject_armed;    // commit on `other` after the next app commit
static int injected;

static int wal_hook(void *ctx, sqlite3 *h, const char *zdb, int frames) {
  (void)ctx; (void)h; (void)zdb; (void)frames;
  if (inject_armed) {
    inject_armed = 0;
    int rc = sqlite3_exec(other, "INSERT INTO noise VALUES (NULL)", NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    injected++;
  }
  return SQLITE_OK;
}

static void cleanup(void) {
  remove(DB_PATH); remove(DB_PATH "-wal"); remove(DB_PATH "-shm");
}

static int count(sqlite3 *h, const char *sql) {
  sqlite3_stmt *s = NULL;
  assert(sqlite3_prepare_v2(h, sql, -1, &s, NULL) == SQLITE_OK);
  int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
  sqlite3_finalize(s);
  return n;
}

// Leave a SELECT open on the application connection (one row read).
static int open_reader(arkilian *db) {
  assert(db_prepare(db, "SELECT id FROM items") == SQLITE_OK);
  int idx = db_stmt_count(db) - 1;
  assert(db_step(db) == SQLITE_ROW);
  return idx;
}

static void close_reader(arkilian *db, int idx) {
  assert(db_use_stmt(db, idx) == SQLITE_OK);
  db_finalize(db);
}

// Run one DDL with the concurrent commit injected, then check that it was
// captured (or that it failed as a whole).
static void ddl_case(arkilian *db, const char *label, const char *ddl,
                     int via_exec, const char *trigger_name,
                     const char *outbox_like) {
  sqlite3 *h = db_get_handle(db);
  int reader = open_reader(db);
  int before_err = n_errors;
  sqlite3_int64 rowid_before = db_last_insert_rowid(db);

  inject_armed = 1;
  int rc;
  if (via_exec) {
    rc = db_exec(db, ddl);
  } else {
    assert(db_prepare(db, ddl) == SQLITE_OK);
    rc = db_step(db);
    db_finalize(db);
  }
  inject_armed = 0;
  close_reader(db, reader);

  char q[256];
  snprintf(q, sizeof(q),
           "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name='%s'",
           trigger_name);
  int has_trigger = count(h, q);
  snprintf(q, sizeof(q),
           "SELECT count(*) FROM _pending_backup WHERE payload LIKE '%s'", outbox_like);
  int captured = count(h, q);

  printf("  %-34s rc=%d trigger=%d outbox=%d errors=%d\n", label, rc,
         has_trigger, captured, n_errors - before_err);

  if (rc == SQLITE_DONE || rc == SQLITE_OK) {
    // Committed → must be fully captured, with no capture errors.
    assert(has_trigger == 1);
    assert(captured == 1);
    assert(n_errors == before_err);
  } else {
    // Failed → must not have committed anything (the app can retry).
    assert(captured == 0);
  }
  assert(db_last_insert_rowid(db) == rowid_before);
}

int main(void) {
  printf("=== DDL capture under a concurrent writer ===\n");
  cleanup();
  setenv("ARKILIAN_ENABLE_BACKUP", "0", 1);

  arkilian *db = NULL;
  assert(db_init(&db, DB_PATH) == SQLITE_OK);
  db_set_log_callback(db, log_cb, NULL);
  assert(db_exec(db, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)") == SQLITE_OK);
  for (int i = 0; i < 10; i++)
    assert(db_exec(db, "INSERT INTO items (name) VALUES ('x')") == SQLITE_OK);
  assert(db_exec(db, "CREATE TABLE noise (x INTEGER PRIMARY KEY)") == SQLITE_OK);

  assert(sqlite3_open(DB_PATH, &other) == SQLITE_OK);
  sqlite3_busy_timeout(other, 5000);
  sqlite3_wal_hook(db_get_handle(db), wal_hook, NULL);

  // Baseline: prepare/step DDL with nothing else going on.
  n_bypass = 0;
  assert(db_prepare(db, "CREATE TABLE plain (id INTEGER PRIMARY KEY, v)") == SQLITE_OK);
  assert(db_step(db) == SQLITE_DONE);
  db_finalize(db);
  printf("  bypass warnings for wrapped prepare/step DDL: %d\n", n_bypass);
  assert(n_bypass == 0);

  ddl_case(db, "CREATE TABLE via prepare/step",
           "CREATE TABLE t_step (id INTEGER PRIMARY KEY, v TEXT)", 0,
           "trg_t_step_ai", "CREATE TABLE t_step%");
  ddl_case(db, "ALTER TABLE via prepare/step",
           "ALTER TABLE items ADD COLUMN extra TEXT", 0,
           "trg_items_ai", "ALTER TABLE items ADD COLUMN extra%");
  ddl_case(db, "CREATE TABLE via db_exec",
           "CREATE TABLE t_exec (id INTEGER PRIMARY KEY, v TEXT)", 1,
           "trg_t_exec_ai", "CREATE TABLE t_exec%");
  ddl_case(db, "ALTER TABLE via db_exec",
           "ALTER TABLE items ADD COLUMN extra2 TEXT", 1,
           "trg_items_ai", "ALTER TABLE items ADD COLUMN extra2%");
  assert(injected >= 1);

  // A row written after the ALTERs must carry the new columns.
  sqlite3 *h = db_get_handle(db);
  int has_extra = count(h, "SELECT count(*) FROM pragma_table_info('items') WHERE name='extra'");
  if (has_extra) {
    assert(db_exec(db, "INSERT INTO items (name, extra) VALUES ('y', 'NEWCOL')") == SQLITE_OK);
    int ok = count(h, "SELECT count(*) FROM _pending_backup WHERE payload LIKE '%NEWCOL%'");
    printf("  row after ALTER captured with new column: %s\n", ok == 1 ? "yes" : "NO");
    assert(ok == 1);
  }

  // The DDL itself on a stale snapshot (another connection committed
  // after the open read began): SQLite refuses the write. It must fail as
  // a whole, leave no savepoint behind, and succeed on retry.
  {
    int reader = open_reader(db);
    assert(sqlite3_exec(other, "INSERT INTO noise VALUES (NULL)", NULL, NULL, NULL) == SQLITE_OK);
    assert(db_prepare(db, "CREATE TABLE t_retry (id INTEGER PRIMARY KEY)") == SQLITE_OK);
    int rc = db_step(db);
    db_finalize(db);
    close_reader(db, reader);
    printf("  DDL on a stale snapshot: rc=%d (%s)\n", rc, db_errmsg(db));
    assert((rc & 0xff) == SQLITE_BUSY);
    assert(sqlite3_get_autocommit(h) == 1);  // no dangling savepoint
    assert(count(h, "SELECT count(*) FROM sqlite_master WHERE name='t_retry'") == 0);
    assert(db_exec(db, "CREATE TABLE t_retry (id INTEGER PRIMARY KEY)") == SQLITE_OK);
    assert(count(h, "SELECT count(*) FROM sqlite_master WHERE name='trg_t_retry_ai'") == 1);
  }

  // Inside an explicit transaction the same rules hold.
  assert(db_begin(db) == SQLITE_OK);
  assert(db_prepare(db, "CREATE TABLE t_tx (id INTEGER PRIMARY KEY)") == SQLITE_OK);
  assert(db_step(db) == SQLITE_DONE);
  db_finalize(db);
  assert(db_commit(db) == SQLITE_OK);
  assert(count(h, "SELECT count(*) FROM sqlite_master WHERE name='trg_t_tx_ai'") == 1);
  assert(count(h, "SELECT count(*) FROM _pending_backup WHERE payload LIKE 'CREATE TABLE t_tx%'") == 1);

  assert(db_backup_triggers_dirty(db) == 0);
  sqlite3_wal_hook(db_get_handle(db), NULL, NULL);
  sqlite3_close(other);
  db_close(db);
  cleanup();
  printf("=== PASS ===\n");
  return 0;
}
