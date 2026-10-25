// Fair Arkilian vs raw SQLite micro-benchmark.
// Both engines: same schema, same pragmas, prepared statements on the
// connection (Arkilian: its application handle, so capture triggers fire).
// Arkilian's outbox is drained OUTSIDE the timed region every 1000 ops,
// simulating a flush thread that keeps up (steady state, small backlog).
// Latency percentiles are exact (sorted samples), not histogram buckets.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "class.h"
#include "sqlite3.h"

static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static int cmpd(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
static void rmdb(const char*p){char t[256];const char*s[]={"","-wal","-shm",".arkddlqueue"};for(int i=0;i<4;i++){snprintf(t,sizeof t,"%s%s",p,s[i]);remove(t);}}
static unsigned rng=42; static unsigned xr(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}

static arkilian *ark=NULL; static sqlite3 *h=NULL; static int is_ark=0;
static void drain(void){ if(is_ark) sqlite3_exec(h,"DELETE FROM _pending_backup",0,0,0); }

static void report(const char*eng,const char*sync,const char*w,double*lat,int n,double total_ns,long rows){
  qsort(lat,n,sizeof(double),cmpd);
  printf("%s,%s,%s,%.0f,%.1f,%.1f,%.1f\n",eng,sync,w,rows/(total_ns/1e9),
         lat[n/2]/1e3,lat[(int)(n*0.95)]/1e3,lat[(int)(n*0.99)]/1e3);
  fflush(stdout);
}

int main(int argc,char**argv){
  int N=argc>1?atoi(argv[1]):20000; const char*sync=argc>2?argv[2]:"NORMAL"; is_ark=argc>3&&!strcmp(argv[3],"ark");
  const char*label=argc>4?argv[4]:(is_ark?"arkilian":"sqlite");
  const char*path="bench_cmp.db"; rmdb(path);
  if(is_ark){
    unsetenv("ARKILIAN_S3_ENDPOINT"); setenv("ARKILIAN_ENABLE_BACKUP","1",1);
    setenv("ARKILIAN_MAX_QUEUE_DEPTH","10000000",1);
    setenv("ARKILIAN_OUTBOX_DURABLE",strcmp(sync,"FULL")?"0":"1",1);
    if(db_init(&ark,path)){fprintf(stderr,"init fail\n");return 1;}
    db_exec(ark,"CREATE TABLE bench(id INTEGER PRIMARY KEY, name TEXT, email TEXT, qty INTEGER, price REAL, note TEXT)");
    h=db_get_handle(ark);
  } else {
    sqlite3_open(path,&h);
    char p[128]; snprintf(p,sizeof p,"PRAGMA journal_mode=WAL; PRAGMA synchronous=%s; PRAGMA busy_timeout=5000; PRAGMA foreign_keys=ON;",sync);
    sqlite3_exec(h,p,0,0,0);
    sqlite3_exec(h,"CREATE TABLE bench(id INTEGER PRIMARY KEY, name TEXT, email TEXT, qty INTEGER, price REAL, note TEXT)",0,0,0);
  }
  sqlite3_stmt *ins,*upd,*sel;
  sqlite3_prepare_v2(h,"INSERT INTO bench VALUES(?,?,?,?,?,?)",-1,&ins,0);
  sqlite3_prepare_v2(h,"UPDATE bench SET qty=?, price=? WHERE id=?",-1,&upd,0);
  sqlite3_prepare_v2(h,"SELECT name,email,qty,price FROM bench WHERE id=?",-1,&sel,0);
  double *lat=malloc(sizeof(double)*(N*10+10)); char name[32],email[48];
  #define BIND_ROW(id) do{snprintf(name,sizeof name,"user-%d",id);snprintf(email,sizeof email,"user%d@example.com",id); \
    sqlite3_bind_int(ins,1,id);sqlite3_bind_text(ins,2,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,3,email,-1,SQLITE_TRANSIENT); \
    sqlite3_bind_int(ins,4,(int)(xr()%100));sqlite3_bind_double(ins,5,(xr()%10000)/100.0);sqlite3_bind_text(ins,6,"standard order",-1,SQLITE_STATIC);}while(0)
  int id=0;
  // warmup
  for(int i=0;i<500;i++){BIND_ROW(++id);sqlite3_step(ins);sqlite3_reset(ins);} drain();
  // W1 single-row INSERT, autocommit
  double tot=0;
  for(int i=0;i<N;i++){ BIND_ROW(++id); double t=now_ns(); sqlite3_step(ins); sqlite3_reset(ins); double d=now_ns()-t; lat[i]=d; tot+=d; if(i%1000==999) drain(); }
  report(label,sync,"insert_autocommit",lat,N,tot,N); drain();
  // W2 batched INSERT, 1000 rows per txn
  int batches=N/1000<20?20:N/1000; tot=0;
  for(int b=0;b<batches;b++){ double t=now_ns(); sqlite3_exec(h,"BEGIN",0,0,0);
    for(int j=0;j<1000;j++){BIND_ROW(++id);sqlite3_step(ins);sqlite3_reset(ins);}
    sqlite3_exec(h,"COMMIT",0,0,0); double d=now_ns()-t; lat[b]=d/1000.0; tot+=d; drain(); }
  report(label,sync,"insert_batch1000",lat,batches,tot,(long)batches*1000);
  // W3 UPDATE by PK, autocommit
  tot=0;
  for(int i=0;i<N;i++){ int k=1+(int)(xr()%id); sqlite3_bind_int(upd,1,(int)(xr()%100)); sqlite3_bind_double(upd,2,(xr()%10000)/100.0); sqlite3_bind_int(upd,3,k);
    double t=now_ns(); sqlite3_step(upd); sqlite3_reset(upd); double d=now_ns()-t; lat[i]=d; tot+=d; if(i%1000==999) drain(); }
  report(label,sync,"update_autocommit",lat,N,tot,N); drain();
  // W4 point SELECT by PK
  int M=N*5; tot=0;
  for(int i=0;i<M;i++){ int k=1+(int)(xr()%id); sqlite3_bind_int(sel,1,k); double t=now_ns(); while(sqlite3_step(sel)==SQLITE_ROW){} sqlite3_reset(sel); double d=now_ns()-t; lat[i]=d; tot+=d; }
  report(label,sync,"select_pk",lat,M,tot,M);
  sqlite3_finalize(ins);sqlite3_finalize(upd);sqlite3_finalize(sel);
  if(is_ark) db_close(ark); else sqlite3_close(h);
  rmdb(path); return 0;
}
