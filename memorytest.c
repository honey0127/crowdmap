/**
 * CrowdMap 서버 부하 테스트 (memorytest.c)
 *
 * 빌드:   gcc -O2 -o memorytest memorytest.c -lpthread -lm
 * 실행:   ./memorytest 127.0.0.1 5000 20
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <math.h>
#include <dirent.h>

#define MAX_EVENTS          4096
#define RECV_BUF_SIZE       256
#define SERVER_PORT         5001
#define MAX_LATENCY_SAMPLES 200000

/* 상황 1: 같은 구역 반복 (캐시 효과) */
static const double LAT_A[] = {
    35.8863,35.8864,35.8865,35.8866,35.8867,
    35.8704,35.8705,35.8706,35.8707,35.8708
};
static const double LNG_A[] = {
    128.6095,128.6096,128.6097,128.6098,128.6099,
    128.5955,128.5956,128.5957,128.5958,128.5959
};
#define LOCS_A 10

/* 상황 2: 다양한 구역 분산 (락 경합) */
static const double LAT_B[] = {
    35.8863,35.8704,35.8796,35.8660,35.8575,
    35.8714,35.8430,35.9000,35.8561,35.8500,
    37.4979,37.5636,37.5538,37.5547,37.5133,
    36.3504,35.1796,37.4563,36.9910,35.5384
};
static const double LNG_B[] = {
    128.6095,128.5955,128.6281,128.5950,128.6305,
    128.5940,128.5330,128.5890,128.6270,128.5500,
    127.0276,126.9839,126.9236,126.9706,127.1000,
    127.3845,129.0756,126.7052,127.7306,129.3114
};
#define LOCS_B 20

static _Atomic long g_connected=0,g_conn_fail=0;
static _Atomic long g_update_sent=0,g_query_sent=0,g_query_recv=0;
static _Atomic long g_total_latency=0,g_latency_count=0,g_latency_max=0;
static long         g_samples[MAX_LATENCY_SAMPLES];
static _Atomic long g_sample_idx=0;
static pthread_mutex_t g_mtx=PTHREAD_MUTEX_INITIALIZER;

static long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000L+ts.tv_nsec/1000000L;
}
static void set_nonblock(int fd){fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);}
static int cmp_long(const void*a,const void*b){long x=*(long*)a,y=*(long*)b;return(x>y)-(x<y);}

/* RSS: /proc 스캔으로 서버 프로세스 찾기 */
static long get_server_rss_kb(void){
    DIR *d=opendir("/proc"); if(!d) return -1;
    struct dirent *e; long rss=-1;
    while((e=readdir(d))!=NULL){
        char *p=e->d_name; int ok=1;
        for(;*p;p++) if(*p<'0'||*p>'9'){ok=0;break;}
        if(!ok) continue;
        char cp[256]; snprintf(cp,sizeof(cp),"/proc/%s/cmdline",e->d_name);
        FILE *cf=fopen(cp,"r"); if(!cf) continue;
        char cmd[512]={0}; size_t r=fread(cmd,1,sizeof(cmd)-1,cf); fclose(cf);
        (void)r;
        if(!strstr(cmd,"crowdmap")&&!strstr(cmd,"server")){continue;}
        if(strstr(cmd,"memorytest")) continue;
        char sp[256]; snprintf(sp,sizeof(sp),"/proc/%s/status",e->d_name);
        FILE *sf=fopen(sp,"r"); if(!sf) continue;
        char line[256];
        while(fgets(line,sizeof(line),sf))
            if(strncmp(line,"VmRSS:",6)==0){sscanf(line+6,"%ld",&rss);break;}
        fclose(sf);
        if(rss>0) break;
    }
    closedir(d); return rss;
}

/* 서버 로그 파싱 */
static void parse_log(long *hit,long *miss,long *fallback){
    *hit=*miss=*fallback=0;
    FILE *fp=popen("ls -t ~/crowdmap/logs/server_*.log 2>/dev/null|head -1","r");
    if(!fp) return;
    char path[256]={0};
    if(!fgets(path,sizeof(path),fp)){pclose(fp);return;}
    pclose(fp); path[strcspn(path,"\n")]='\0';
    if(!strlen(path)) return;
    FILE *lf=fopen(path,"r"); if(!lf) return;
    char line[512];
    while(fgets(line,sizeof(line),lf)){
        if(strstr(line,"cache HIT"))       (*hit)++;
        else if(strstr(line,"cache MISS")) (*miss)++;
        else if(strstr(line,"Fallback"))   (*fallback)++;
    }
    fclose(lf);
}

static void adjust_ulimit(int need){
    struct rlimit rl; getrlimit(RLIMIT_NOFILE,&rl);
    if((long)rl.rlim_cur<need+64){
        rl.rlim_cur=(rl.rlim_max>(rlim_t)(need+64))?(rlim_t)(need+64):rl.rlim_max;
        setrlimit(RLIMIT_NOFILE,&rl);
    }
}

typedef enum{CONN,ACTIVE}State;
typedef struct{
    int fd,user_id,action,scenario;
    State state; long qtime;
    char rbuf[RECV_BUF_SIZE]; size_t rlen;
}Client;
typedef struct{const char*host;int start_id,count,duration,scenario;}WorkerArg;

static void send_pkt(int fd,int uid,int loc,int scenario){
    double lat,lng;
    if(scenario==0){lat=LAT_A[loc%LOCS_A]+(rand()%10-5)*0.00001;lng=LNG_A[loc%LOCS_A]+(rand()%10-5)*0.00001;}
    else{lat=LAT_B[loc%LOCS_B];lng=LNG_B[loc%LOCS_B];}
    char buf[64]; int n=snprintf(buf,sizeof(buf),"%d,%.6f,%.6f\n",uid,lat,lng);
    (void)write(fd,buf,(size_t)n);
}

static void do_recv(Client *c){
    while(1){
        ssize_t n=read(c->fd,c->rbuf+c->rlen,RECV_BUF_SIZE-c->rlen-1);
        if(n<=0) break;
        c->rlen+=(size_t)n; c->rbuf[c->rlen]='\0';
        char *nl;
        while((nl=memchr(c->rbuf,'\n',c->rlen))!=NULL){
            if(c->qtime>0){
                long lat=now_ms()-c->qtime;
                atomic_fetch_add(&g_query_recv,1);
                atomic_fetch_add(&g_total_latency,lat);
                atomic_fetch_add(&g_latency_count,1);
                long prev=atomic_load(&g_latency_max);
                while(lat>prev) if(atomic_compare_exchange_weak(&g_latency_max,&prev,lat)) break;
                long idx=atomic_fetch_add(&g_sample_idx,1);
                if(idx<MAX_LATENCY_SAMPLES){pthread_mutex_lock(&g_mtx);g_samples[idx]=lat;pthread_mutex_unlock(&g_mtx);}
                c->qtime=0;
            }
            size_t used=(size_t)(nl-c->rbuf)+1;
            memmove(c->rbuf,c->rbuf+used,c->rlen-used);
            c->rlen-=used;
        }
    }
}

static void *worker(void *arg){
    WorkerArg *wa=(WorkerArg*)arg;
    Client *clients=calloc((size_t)wa->count,sizeof(Client));
    if(!clients) return NULL;
    int epfd=epoll_create1(0);
    struct epoll_event ev,events[MAX_EVENTS];
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons(SERVER_PORT);
    inet_pton(AF_INET,wa->host,&addr.sin_addr);
    for(int i=0;i<wa->count;i++){
        int fd=socket(AF_INET,SOCK_STREAM,0);
        if(fd<0){atomic_fetch_add(&g_conn_fail,1);clients[i].fd=-1;continue;}
        set_nonblock(fd);
        clients[i].fd=fd;clients[i].user_id=wa->start_id+i+1;
        clients[i].state=CONN;clients[i].scenario=wa->scenario;
        int r=connect(fd,(struct sockaddr*)&addr,sizeof(addr));
        if(r<0&&errno!=EINPROGRESS){atomic_fetch_add(&g_conn_fail,1);close(fd);clients[i].fd=-1;continue;}
        ev.events=EPOLLIN|EPOLLOUT|EPOLLET;ev.data.ptr=&clients[i];
        epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev);
        if(i%100==99) usleep(10000);
    }
    long end=now_ms()+(long)wa->duration*1000L;
    while(now_ms()<end){
        int nev=epoll_wait(epfd,events,MAX_EVENTS,200);
        for(int i=0;i<nev;i++){
            Client *c=(Client*)events[i].data.ptr;
            if(!c||c->fd<0) continue;
            if(events[i].events&EPOLLOUT&&c->state==CONN){
                int err=0;socklen_t l=sizeof(err);
                getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&err,&l);
                if(err!=0){atomic_fetch_add(&g_conn_fail,1);epoll_ctl(epfd,EPOLL_CTL_DEL,c->fd,NULL);close(c->fd);c->fd=-1;continue;}
                c->state=ACTIVE;atomic_fetch_add(&g_connected,1);
                ev.events=EPOLLIN|EPOLLET;ev.data.ptr=c;epoll_ctl(epfd,EPOLL_CTL_MOD,c->fd,&ev);
            }
            if(events[i].events&EPOLLIN) do_recv(c);
        }
        for(int i=0;i<wa->count;i++){
            Client *c=&clients[i];
            if(c->fd<0||c->state!=ACTIVE) continue;
            int loc=(c->user_id+c->action)%(wa->scenario==0?LOCS_A:LOCS_B);
            if(c->action%5==0){send_pkt(c->fd,0,loc,wa->scenario);c->qtime=now_ms();atomic_fetch_add(&g_query_sent,1);}
            else{send_pkt(c->fd,c->user_id,loc,wa->scenario);atomic_fetch_add(&g_update_sent,1);}
            c->action++;
        }
    }
    for(int i=0;i<wa->count;i++) if(clients[i].fd>=0) close(clients[i].fd);
    free(clients);close(epfd);return NULL;
}

static void reset(void){
    atomic_store(&g_connected,0);atomic_store(&g_conn_fail,0);
    atomic_store(&g_update_sent,0);atomic_store(&g_query_sent,0);
    atomic_store(&g_query_recv,0);atomic_store(&g_total_latency,0);
    atomic_store(&g_latency_count,0);atomic_store(&g_latency_max,0);
    atomic_store(&g_sample_idx,0);memset(g_samples,0,sizeof(g_samples));
}

typedef struct{int n;long tps,avg,p99,max_lat,rss_kb;long hit,miss,fallback;}R;

static R run(const char *host,int n,int dur,int scenario,long rss_base){
    R r={0};r.n=n;reset();
    int nt=(n<=100)?2:4;
    int per=n/nt,rem=n%nt;
    pthread_t th[64];WorkerArg wa[64];
    long t0=now_ms();
    for(int i=0;i<nt;i++){
        wa[i].host=host;wa[i].start_id=i*per;
        wa[i].count=per+(i==nt-1?rem:0);
        wa[i].duration=dur;wa[i].scenario=scenario;
        pthread_create(&th[i],NULL,worker,&wa[i]);
    }
    /* 점으로 진행 표시 */
    for(int t=0;t<dur;t++){sleep(1);if(t%4==3){printf(".");fflush(stdout);}}
    for(int i=0;i<nt;i++) pthread_join(th[i],NULL);
    long elapsed=now_ms()-t0;
    long rss_now=get_server_rss_kb();
    r.rss_kb=(rss_now>0&&rss_base>0)?rss_now-rss_base:0;
    long cnt=atomic_load(&g_latency_count);
    r.avg=cnt>0?atomic_load(&g_total_latency)/cnt:0;
    r.max_lat=atomic_load(&g_latency_max);
    long sc=atomic_load(&g_sample_idx);
    if(sc>MAX_LATENCY_SAMPLES)sc=MAX_LATENCY_SAMPLES;
    if(sc>1){qsort(g_samples,(size_t)sc,sizeof(long),cmp_long);r.p99=g_samples[(long)(sc*0.99)];}
    long ops=atomic_load(&g_update_sent)+atomic_load(&g_query_sent);
    r.tps=elapsed>0?(long)((double)ops/((double)elapsed/1000.0)):0;
    parse_log(&r.hit,&r.miss,&r.fallback);
    sleep(2);return r;
}

int main(int argc,char *argv[]){
    signal(SIGPIPE,SIG_IGN);
    const char *host=argc>=2?argv[1]:"127.0.0.1";
    int maxu=argc>=3?atoi(argv[2]):5000;
    int dur=argc>=4?atoi(argv[3]):20;
    adjust_ulimit(maxu+128);

    long rss_init=get_server_rss_kb();

    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║        CrowdMap 서버 부하 테스트              ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  서버: %-10s  포트: %-5d                ║\n",host,SERVER_PORT);
    printf("║  최대접속: %-5d명   단계시간: %-3d초          ║\n",maxu,dur);
    if(rss_init>0)
        printf("║  서버 초기 RSS: %ld KB (%.1f MB)%*s║\n",
               rss_init,(double)rss_init/1024.0,
               (int)(14-snprintf(NULL,0,"%ld KB (%.1f MB)",rss_init,(double)rss_init/1024.0)),"");
    printf("╚══════════════════════════════════════════════╝\n\n");


    int steps[]={50,500,5000,50000,100000};
    int ns=5;
    if(maxu<50000)ns=4;
    if(maxu<5000)ns=3;
    if(maxu<500)ns=2;
    if(maxu<50)ns=1;
    steps[ns-1]=maxu;
    R rA[5],rB[5];

    printf("▶ 상황 1 — 캐시 효과 측정 (같은 구역 반복 조회)\n");
    for(int s=0;s<ns;s++){
        printf("  [%5d명] ",steps[s]);
        rA[s]=run(host,steps[s],dur,0,rss_init);
        printf(" 완료\n");
    }

    printf("\n▶ 상황 2 — 동시 접속 부하 측정 (다양한 구역 분산)\n");
    for(int s=0;s<ns;s++){
        printf("  [%5d명] ",steps[s]);
        rB[s]=run(host,steps[s],dur,1,rss_init);
        printf(" 완료\n");
    }

    /* ── 최종 결과 출력 ── */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                      최종 결과 요약                              ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");

    /* 상황 1 */
    printf("║  [상황 1] 캐시 효과 — 같은 구역 반복 조회                        ║\n");
    printf("╠════════════╦══════════╦══════════╦══════════╦════════════╦══════╣\n");
    printf("║  접속 수   ║   TPS    ║ avg(ms)  ║ p99(ms)  ║ RSS증가(MB)║히트율║\n");
    printf("╠════════════╬══════════╬══════════╬══════════╬════════════╬══════╣\n");
    for(int s=0;s<ns;s++){
        R *r=&rA[s];
        long tot=r->hit+r->miss;
        double hr=tot>0?100.0*r->hit/tot:0.0;
        double rss_mb=(double)r->rss_kb/1024.0;
        printf("║ %10d ║ %8ld ║ %8ld ║ %8ld ║ %+10.1f ║%5.0f%%║\n",
               r->n,r->tps,r->avg,r->p99,rss_mb,hr);
    }
    printf("╠════════════╩══════════╩══════════╩══════════╩════════════╩══════╣\n");

    /* 상황 2 */
    printf("║  [상황 2] 동시 접속 부하 — 다양한 구역 분산 조회                 ║\n");
    printf("╠════════════╦══════════╦══════════╦══════════╦════════════╦══════╣\n");
    printf("║  접속 수   ║   TPS    ║ avg(ms)  ║ p99(ms)  ║ RSS증가(MB)║FB횟수║\n");
    printf("╠════════════╬══════════╬══════════╬══════════╬════════════╬══════╣\n");
    for(int s=0;s<ns;s++){
        R *r=&rB[s];
        double rss_mb=(double)r->rss_kb/1024.0;
        printf("║ %10d ║ %8ld ║ %8ld ║ %8ld ║ %+10.1f ║%6ld║\n",
               r->n,r->tps,r->avg,r->p99,rss_mb,r->fallback);
    }
    printf("╠════════════╩══════════╩══════════╩══════════╩════════════╩══════╣\n");

    /* 메모리 요약 */
    long rss_final=get_server_rss_kb();
    double rss_init_mb=(double)rss_init/1024.0;
    double rss_final_mb=rss_final>0?(double)rss_final/1024.0:0.0;
    printf("║  [메모리 요약]                                                    ║\n");
    printf("║  초기 RSS: %.1f MB  →  최종 RSS: %.1f MB  (증가: +%.1f MB)%*s║\n",
           rss_init_mb, rss_final_mb, rss_final_mb-rss_init_mb,
           (int)(5-snprintf(NULL,0,"%.1f",rss_final_mb-rss_init_mb)),"");
    printf("║  MemoryPool 고정: 32MB  /  2GB 상한 대비 여유: %.0fx            ║\n",
           rss_final>0?2.0*1024.0*1024.0/rss_final:0.0);
    if(rss_final>0&&rss_final<2L*1024*1024)
        printf("║  판정: 2GB 상한 이내 OK ✓                                        ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n=== 테스트 완료 ===\n");
    return 0;
}