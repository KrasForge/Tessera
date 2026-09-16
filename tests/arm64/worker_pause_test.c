#include "audio_worker.h"
#include <pthread.h>
#include <threads.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static audio_worker_t worker;
static uint32_t finish;
static uint64_t calls;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"PAUSE FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void node(void *p) { (void)p; __atomic_fetch_add(&calls,1u,__ATOMIC_RELAXED); }
static void *consume(void *p) { aw_worker_loop(p); return NULL; }
static void *produce(void *p)
{
    audio_worker_t *w=p; uint64_t seq=0;
    while (!__atomic_load_n(&finish,__ATOMIC_ACQUIRE)) {
        ++seq; aw_kick_at(w,seq,seq*1000);
        thrd_yield();
    }
    return NULL;
}
int main(void)
{
    aw_init(&worker,1); CHECK(aw_assign(&worker,node,NULL)==0);
    pthread_t a,b; CHECK(!pthread_create(&a,NULL,consume,&worker));
    CHECK(!pthread_create(&b,NULL,produce,&worker));
    for (unsigned i=0;i<10000;++i) {
        aw_pause(&worker);
        while (!aw_paused(&worker)) thrd_yield();
        uint64_t old=__atomic_load_n(&calls,__ATOMIC_RELAXED);
        CHECK(aw_quiescent(&worker));
        thrd_yield();
        CHECK(__atomic_load_n(&calls,__ATOMIC_RELAXED)==old);
        /* Swap a node while the cadence producer is still calling kick. */
        aw_clear(&worker); CHECK(aw_assign(&worker,node,NULL)==0);
        CHECK(aw_resume(&worker)==0);
    }
    aw_pause(&worker); while(!aw_paused(&worker)) thrd_yield();
    __atomic_store_n(&finish,1u,__ATOMIC_RELEASE);
    CHECK(!pthread_join(b,NULL)); aw_stop(&worker); CHECK(!pthread_join(a,NULL));
    CHECK(!worker.online && aw_quiescent(&worker));
    printf("WORKER PAUSE: PASS (%u checks; 10000 racing pause/resume cycles)\n",checks);
}
