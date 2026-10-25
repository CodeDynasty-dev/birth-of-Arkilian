// P0 replication-correctness regressions (2026-10 production evaluation).
//
// Each scenario failed on v1.5.0 and is fixed by this release:
//   S1  UPDATE that changes a PRIMARY KEY left a ghost row in the replica
//       (INTEGER, TEXT and composite WITHOUT ROWID keys).
//   S2  Outbox overflow silently captured part of a transaction; now a
//       durable capture-gap marker is written in the same transaction (and
//       strict mode, ARKILIAN_CAPTURE_OVERFLOW=abort, fails the write).
//   S3  Restore -> keep writing -> restore again failed (the restored node
//       restarted LSNs at 1) — checked across four generations, including a
//       snapshot taken on a restored node.
//   S4  A second writer / un-restored node on the same prefix is detected
//       and frozen instead of publishing an interleaved history.
//   S5  DDL captured just before a snapshot was replayed on top of an image
//       that already contained it, failing the restore; also checks that a
//       clean shutdown publishes its final chunks.
#include "class.h"
#include "hydration.h"
#include "sha256.h"
#include "ark_stub_s3.h"
#include "deps/sqlite/sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void rmdb(const char *p) {
  char t[256];
  const char *sfx[] = {"", "-wal", "-shm", ".arkddlqueue", "-journal"};
  for (int i = 0; i < 5; i++) {
    snprintf(t, sizeof(t), "%s%s", p, sfx[i]);
    remove(t);
  }
}

// Canonical dump of a table: every row, every column, ordered by the first
// column. Equality of dumps == logical equality of the table.
static char *dump_table(const char *db_path, const char *table) {
  sqlite3 *h = NULL;
  if (sqlite3_open_v2(db_path, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    sqlite3_close(h);
    return strdup("<open failed>");
  }
  char *q = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY 1, 2", table);
  sqlite3_stmt *st = NULL;
  size_t cap = 4096, len = 0;
  char *out = malloc(cap);
  out[0] = 0;
  if (sqlite3_prepare_v2(h, q, -1, &st, NULL) != SQLITE_OK) {
    snprintf(out, cap, "<prepare failed: %s>", sqlite3_errmsg(h));
  } else {
    while (sqlite3_step(st) == SQLITE_ROW) {
      for (int i = 0; i < sqlite3_column_count(st); i++) {
        const char *v = (const char *)sqlite3_column_text(st, i);
        size_t need = len + (v ? strlen(v) : 4) + 4;
        if (need >= cap) { cap = need * 2; out = realloc(out, cap); }
        len += (size_t)sprintf(out + len, "%s|", v ? v : "NULL");
      }
      if (len + 2 >= cap) { cap *= 2; out = realloc(out, cap); }
      out[len++] = '\n';
      out[len] = 0;
    }
  }
  sqlite3_finalize(st);
  sqlite3_free(q);
  sqlite3_close(h);
  return out;
}

static void assert_same_table(const char *a, const char *b, const char *table) {
  char *da = dump_table(a, table);
  char *dbb = dump_table(b, table);
  if (strcmp(da, dbb) != 0) {
    fprintf(stderr, "TABLE MISMATCH %s:\n--- %s\n%s--- %s\n%s", table, a, da, b, dbb);
    assert(0 && "restored table differs from source");
  }
  free(da);
  free(dbb);
}

static long long q1(sqlite3 *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  long long v = -1;
  if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

// ── S1: PK changes replay faithfully ─────────────────────────────────
static void pk_case(const char *name, const char *schema, const char *seed,
                    const char *work, const char *table) {
  char src[64], dst[64];
  snprintf(src, sizeof(src), "p0r_%s_src.db", name);
  snprintf(dst, sizeof(dst), "p0r_%s_dst.db", name);
  rmdb(src);
  rmdb(dst);
  arkilian *db = NULL;
  assert(db_init(&db, src) == 0);
  assert(db_exec(db, schema) == SQLITE_OK);
  assert(db_exec(db, seed) == SQLITE_OK);
  sqlite3 *h = db_get_handle(db);
  // Replica = state after seed (what a snapshot would contain).
  sqlite3 *r = NULL;
  assert(sqlite3_open(dst, &r) == SQLITE_OK);
  assert(sqlite3_exec(r, schema, NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(r, seed, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_exec(h, "DELETE FROM _pending_backup", NULL, NULL, NULL);

  assert(db_exec(db, work) == SQLITE_OK);
  // Ship exactly what the outbox captured, as one chunk.
  sqlite3_stmt *st = NULL;
  assert(sqlite3_prepare_v2(h, "SELECT payload FROM _pending_backup ORDER BY id",
                            -1, &st, NULL) == SQLITE_OK);
  char body[8192];
  size_t off = 0;
  while (sqlite3_step(st) == SQLITE_ROW)
    off += (size_t)snprintf(body + off, sizeof(body) - off, "%s;\n",
                            sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  assert(hydrate_replay_chunk(r, body, 1000) == 0);
  sqlite3_close(r);
  db_close(db);
  assert_same_table(src, dst, table);
  rmdb(src);
  rmdb(dst);
  printf("  S1 %-28s replica == source\n", name);
}

static void s1_pk_changes(void) {
  clear_s3_env();
  pk_case("int_pk", "CREATE TABLE u(id INTEGER PRIMARY KEY, name TEXT)",
          "INSERT INTO u VALUES(1,'alice'),(2,'bob')",
          "UPDATE u SET id = 10 WHERE id = 1; UPDATE u SET id = id + 100", "u");
  pk_case("text_pk", "CREATE TABLE acct(handle TEXT PRIMARY KEY, bal INT)",
          "INSERT INTO acct VALUES('bob',100)",
          "UPDATE acct SET handle = 'robert' WHERE handle = 'bob'", "acct");
  pk_case("composite_without_rowid",
          "CREATE TABLE kv(a TEXT, b INT, v TEXT, PRIMARY KEY(a,b)) WITHOUT ROWID",
          "INSERT INTO kv VALUES('x',1,'old'),('y',1,'keep')",
          "UPDATE kv SET b = 2 WHERE a = 'x'; UPDATE kv SET v = 'new' WHERE a = 'y'", "kv");
  pk_case("pk_swap_in_txn", "CREATE TABLE s(id INTEGER PRIMARY KEY, n TEXT)",
          "INSERT INTO s VALUES(1,'one'),(2,'two')",
          "BEGIN; UPDATE s SET id = 3 WHERE id = 1; UPDATE s SET id = 1 WHERE id = 2; "
          "UPDATE s SET id = 2 WHERE id = 3; COMMIT", "s");
}

// ── S2: overflow is never silent ─────────────────────────────────────
static void s2_overflow(void) {
  clear_s3_env();
  setenv("ARKILIAN_ENABLE_BACKUP", "1", 1);
  setenv("ARKILIAN_MAX_QUEUE_DEPTH", "5", 1);
  rmdb("p0r_cap.db");
  arkilian *db = NULL;
  assert(db_init(&db, "p0r_cap.db") == 0);
  assert(db_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INT)") == SQLITE_OK);
  sqlite3 *h = db_get_handle(db);
  sqlite3_exec(h, "DELETE FROM _pending_backup", NULL, NULL, NULL);
  assert(db_backup_capture_paused(db) == 0);
  assert(db_begin(db) == SQLITE_OK);
  for (int i = 1; i <= 10; i++) {
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,%d)", i, i);
    assert(db_exec(db, sql) == SQLITE_OK);
  }
  assert(db_commit(db) == SQLITE_OK);
  assert(q1(h, "SELECT count(*) FROM t") == 10);
  assert(q1(h, "SELECT count(*) FROM _pending_backup") <= 5);
  // The gap is durable, in the same transaction, and visible immediately.
  assert(q1(h, "SELECT count(*) FROM _arkilian_meta WHERE k = 'capture_gap'") == 1);
  assert(db_backup_capture_paused(db) == 1);
  assert((db_backup_health_flags(db) & ARK_HF_NO_CAPTURE_GAP) == 0);
  // An explicit conflict clause on the application's statement overrides
  // the trigger body's: the bump must still advance, and never fail the
  // application's write.
  long long g0 = q1(h, "SELECT CAST(v AS INTEGER) FROM _arkilian_meta WHERE k = 'capture_gap'");
  assert(db_exec(db, "INSERT OR IGNORE INTO t VALUES(11, 11)") == SQLITE_OK);
  long long g1 = q1(h, "SELECT CAST(v AS INTEGER) FROM _arkilian_meta WHERE k = 'capture_gap'");
  assert(g1 > g0);
  assert(db_exec(db, "INSERT OR ABORT INTO t VALUES(12, 12)") == SQLITE_OK);
  assert(db_exec(db, "INSERT OR ROLLBACK INTO t VALUES(13, 13)") == SQLITE_OK);
  assert(db_exec(db, "UPDATE OR FAIL t SET v = v + 1 WHERE id = 1") == SQLITE_OK);
  long long g2 = q1(h, "SELECT CAST(v AS INTEGER) FROM _arkilian_meta WHERE k = 'capture_gap'");
  assert(g2 > g1);
  assert(q1(h, "SELECT count(*) FROM t") == 13);
  db_close(db);
  rmdb("p0r_cap.db");
  printf("  S2 overflow writes a durable capture-gap marker (health RED)\n");

  // Strict mode: the write that cannot be captured fails; nothing is torn.
  setenv("ARKILIAN_CAPTURE_OVERFLOW", "abort", 1);
  rmdb("p0r_cap2.db");
  assert(db_init(&db, "p0r_cap2.db") == 0);
  assert(db_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INT)") == SQLITE_OK);
  h = db_get_handle(db);
  sqlite3_exec(h, "DELETE FROM _pending_backup", NULL, NULL, NULL);
  assert(db_begin(db) == SQLITE_OK);
  int failed_at = 0;
  for (int i = 1; i <= 10; i++) {
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,%d)", i, i);
    if (db_exec(db, sql) != SQLITE_OK) { failed_at = i; break; }
  }
  assert(failed_at == 6);
  assert(db_rollback(db) == SQLITE_OK);
  assert(q1(h, "SELECT count(*) FROM t") == 0);
  assert(q1(h, "SELECT count(*) FROM _arkilian_meta WHERE k = 'capture_gap'") == 0);
  db_close(db);
  rmdb("p0r_cap2.db");
  unsetenv("ARKILIAN_CAPTURE_OVERFLOW");
  unsetenv("ARKILIAN_MAX_QUEUE_DEPTH");
  printf("  S2 strict mode (ARKILIAN_CAPTURE_OVERFLOW=abort) fails the write\n");
}

// ── S3/S4/S5 helpers (S3 stub) ───────────────────────────────────────
static void s3_env(void) {
  set_s3_env();
  setenv("ARKILIAN_ENABLE_BACKUP", "1", 1);
  setenv("ARKILIAN_CHUNK_INTERVAL_SEC", "1", 1);
  setenv("ARKILIAN_MANIFEST_INTERVAL_SEC", "1", 1);
  setenv("ARKILIAN_BACKUP_INTERVAL", "999999", 1);
  setenv("ARKILIAN_BACKUP_PATH", "p0r_backup.sqlite", 1);
}

static long long manifest_head(void) {
  char key[512];
  snprintf(key, sizeof(key), "%s/manifest.json", PREFIX);
  char *body = NULL;
  size_t len = 0;
  if (!stub_get(key, &body, &len) || !body) return -1;
  long long head = 0;
  const char *keys[] = {"\"lsn_end\":", "\"baseline_lsn\":"};
  for (int k = 0; k < 2; k++) {
    for (const char *p = strstr(body, keys[k]); p; p = strstr(p + 1, keys[k])) {
      long long v = atoll(p + strlen(keys[k]));
      if (v > head) head = v;
    }
  }
  free(body);
  return head;
}

static long long local_seq(arkilian *db) {
  return q1(db_get_handle(db),
            "SELECT coalesce((SELECT seq FROM sqlite_sequence WHERE name='_pending_backup'),0)");
}

// Wait until every captured change is shipped AND referenced by the
// published manifest.
static void settle(arkilian *db) {
  long long want = local_seq(db);
  for (int i = 0; i < 400; i++) {
    if (db_backup_queue_depth(db) == 0 && manifest_head() >= want) return;
    db_wal_flush(db);
    usleep(50000);
  }
  fprintf(stderr, "settle timeout: queue=%d manifest_head=%lld want=%lld\n",
          db_backup_queue_depth(db), manifest_head(), want);
  assert(0 && "settle timeout");
}

static void writes(arkilian *db, int from, int to) {
  for (int i = from; i <= to; i++) {
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO users(id, name) VALUES(%d, 'u%d')", i, i);
    assert(db_exec(db, sql) == SQLITE_OK);
  }
}

static void restore(const char *dst) {
  rmdb(dst);
  int rc = arkilian_hydrate_s3(dst, g_endpoint, BUCKET, "us-east-1", "test-access",
                               "test-secret", PREFIX, NULL, NULL);
  if (rc != HYDRATION_OK) fprintf(stderr, "restore %s rc=%d\n", dst, rc);
  assert(rc == HYDRATION_OK);
}

// ── S3: failover chain ───────────────────────────────────────────────
static void s3_failover_chain(void) {
  stub_reset();
  s3_env();
  const char *gen[] = {"p0r_gen_a.db", "p0r_gen_b.db", "p0r_gen_c.db", "p0r_gen_d.db"};
  for (int i = 0; i < 4; i++) rmdb(gen[i]);

  arkilian *db = NULL;
  assert(db_init(&db, gen[0]) == 0);
  assert(db_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)") == SQLITE_OK);
  writes(db, 1, 50);
  assert(db_exec(db, "UPDATE users SET id = 1000 WHERE id = 1") == SQLITE_OK);
  settle(db);
  db_close(db);

  int next = 51;
  for (int g = 1; g < 4; g++) {
    restore(gen[g]);
    assert_same_table(gen[g - 1], gen[g], "users");
    assert(db_init(&db, gen[g]) == 0);
    writes(db, next, next + 24);
    next += 25;
    if (g == 2) {
      // Snapshot on a restored node: its image carries a stale
      // last_applied_lsn from its own restore, which used to make the next
      // restore refuse ("snapshot LSN < plan baseline").
      assert(arkilian_run_snapshot_cycle(db) == SQLITE_OK);
      writes(db, next, next + 9);
      next += 10;
    }
    settle(db);
    db_close(db);
    printf("  S3 generation %d restored, continued writing, and shipped\n", g);
  }
  restore("p0r_gen_final.db");
  assert_same_table(gen[3], "p0r_gen_final.db", "users");
  for (int i = 0; i < 4; i++) rmdb(gen[i]);
  rmdb("p0r_gen_final.db");
  printf("  S3 four-generation failover chain restores exactly\n");
}

// ── S4: split-brain guard ────────────────────────────────────────────
static void s4_split_brain(void) {
  // Precondition: S3 left a valid manifest on the stub for PREFIX.
  char key[512];
  snprintf(key, sizeof(key), "%s/manifest.json", PREFIX);
  char *before = NULL;
  size_t blen = 0;
  assert(stub_get(key, &before, &blen) && before);

  rmdb("p0r_intruder.db");
  arkilian *db = NULL;
  assert(db_init(&db, "p0r_intruder.db") == 0);  // NOT restored from the prefix
  assert(db_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)") == SQLITE_OK);
  writes(db, 1, 5);
  for (int i = 0; i < 60; i++) {
    db_wal_flush(db);
    usleep(50000);
  }
  // Frozen: not resolved, unhealthy, nothing shipped, manifest untouched.
  assert((db_backup_health_flags(db) & ARK_HF_MANIFEST_RESOLVED) == 0);
  assert(db_backup_is_healthy(db) == 0);
  assert(db_backup_queue_depth(db) >= 6);
  char *after = NULL;
  size_t alen = 0;
  assert(stub_get(key, &after, &alen) && after);
  assert(alen == blen && memcmp(before, after, blen) == 0);
  free(before);
  free(after);
  db_close(db);
  rmdb("p0r_intruder.db");
  printf("  S4 un-restored second writer is frozen; remote history untouched\n");
}

// ── S5: DDL straddling a snapshot + shutdown publish ─────────────────
static void s5_ddl_straddle(void) {
  stub_reset();
  s3_env();
  // Chunks are held in the buffer (long interval) so the DDL is still
  // unshipped when the snapshot runs: the chunk that later carries it
  // straddles the snapshot baseline.
  setenv("ARKILIAN_CHUNK_INTERVAL_SEC", "3600", 1);
  rmdb("p0r_ddl_src.db");
  arkilian *db = NULL;
  assert(db_init(&db, "p0r_ddl_src.db") == 0);
  assert(stub_manifest_contains("backup.sqlite", 30));  // startup snapshot
  assert(db_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)") == SQLITE_OK);
  writes(db, 1, 10);
  assert(db_exec(db, "ALTER TABLE users ADD COLUMN tier TEXT") == SQLITE_OK);
  assert(db_exec(db, "CREATE INDEX idx_users_name ON users(name)") == SQLITE_OK);
  assert(db_exec(db, "UPDATE users SET tier = 'pro' WHERE id <= 3") == SQLITE_OK);
  // A multi-statement trigger body (';' inside BEGIN...END) must replay as
  // one statement — once inside the snapshot window, once after it.
  assert(db_exec(db, "CREATE TABLE audit(id INTEGER PRIMARY KEY, uid INT, note TEXT)") == SQLITE_OK);
  assert(db_exec(db, "CREATE TRIGGER trg_before AFTER INSERT ON users BEGIN "
                     "INSERT INTO audit(uid, note) VALUES (NEW.id, 'a; b'); "
                     "INSERT INTO audit(uid, note) VALUES (NEW.id, 'c'); END") == SQLITE_OK);
  assert(arkilian_run_snapshot_cycle(db) == SQLITE_OK);
  assert(db_exec(db, "CREATE TRIGGER trg_after AFTER DELETE ON users BEGIN "
                     "DELETE FROM audit WHERE uid = OLD.id; "
                     "INSERT INTO audit(uid, note) VALUES (OLD.id, 'gone;'); END") == SQLITE_OK);
  writes(db, 11, 15);
  assert(db_exec(db, "UPDATE users SET id = 99 WHERE id = 11") == SQLITE_OK);
  db_close(db);  // final drain + immediate manifest publish
  restore("p0r_ddl_dst.db");
  assert_same_table("p0r_ddl_src.db", "p0r_ddl_dst.db", "users");
  assert_same_table("p0r_ddl_src.db", "p0r_ddl_dst.db", "audit");
  {
    sqlite3 *h = NULL;
    assert(sqlite3_open("p0r_ddl_dst.db", &h) == SQLITE_OK);
    assert(q1(h, "SELECT count(*) FROM sqlite_master WHERE type = 'trigger' "
                 "AND name IN ('trg_before', 'trg_after')") == 2);
    sqlite3_close(h);
  }
  rmdb("p0r_ddl_src.db");
  rmdb("p0r_ddl_dst.db");
  setenv("ARKILIAN_CHUNK_INTERVAL_SEC", "1", 1);
  printf("  S5 DDL straddling a snapshot restores; shutdown publishes final chunks\n");
}

// ── S6: a large backlog at shutdown is fully published ──────────────
static void s6_shutdown_backlog(void) {
  stub_reset();
  s3_env();
  setenv("ARKILIAN_CHUNK_INTERVAL_SEC", "3600", 1);  // nothing ships before close
  rmdb("p0r_tail_src.db");
  arkilian *db = NULL;
  assert(db_init(&db, "p0r_tail_src.db") == 0);
  assert(stub_manifest_contains("backup.sqlite", 30));
  assert(db_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)") == SQLITE_OK);
  sqlite3 *h = db_get_handle(db);
  assert(sqlite3_exec(h, "BEGIN; WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 12000) "
                         "INSERT INTO users SELECT x, 'u' || x FROM c; COMMIT;", NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(h, "UPDATE users SET name = name || '!' WHERE id % 3 = 0; "
                         "DELETE FROM users WHERE id % 97 = 0;", NULL, NULL, NULL) == SQLITE_OK);
  db_close(db);  // ~16k captured rows must all ship and be published
  restore("p0r_tail_dst.db");
  assert_same_table("p0r_tail_src.db", "p0r_tail_dst.db", "users");
  rmdb("p0r_tail_src.db");
  rmdb("p0r_tail_dst.db");
  setenv("ARKILIAN_CHUNK_INTERVAL_SEC", "1", 1);
  printf("  S6 a 16k-row backlog at shutdown is fully shipped and restorable\n");
}

int main(void) {
  printf("=== P0 replication regressions ===\n");
  s1_pk_changes();
  s2_overflow();
  stub_start();
  s3_failover_chain();
  s4_split_brain();
  s5_ddl_straddle();
  s6_shutdown_backlog();
  stub_stop();
  remove("p0r_backup.sqlite");
  printf("\nAll P0 replication regressions passed!\n");
  return 0;
}
