/* Exercise the actual control/cadence/transport implementation under pthreads.
 * Only MMU/EL0/lifecycle machinery is stubbed; actual ELF execution is tested
 * separately in QEMU. Control edits race the complete running sample graph. */
#include "audio_session.h"
#include "budget.h"
#include "sample_bits.h"
#include <pthread.h>
#include <threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, allocated, next_pid=1;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"SESSION THREAD FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static audio_session_t app;
static audio_worker_t workers[3];
static uint64_t time_ticks=1000;
static unsigned stop, stop_cadence, bad;
static uint64_t rendered;
struct fake {plugin_t *pl;process_t process;void *input,*output;unsigned gain;uint32_t value;} objects[32];
static unsigned char arena[128*4096];
unsigned char *g_hosttest_ram=arena;
static unsigned char pages[128];
static struct fake *lookup(plugin_t *pl) {return (struct fake *)pl->param_pa;}
uintptr_t phys_alloc_page_zero(void){for(unsigned i=1;i<128;++i)if(!pages[i]){pages[i]=1;++allocated;memset(arena+i*4096,0,4096);return i*4096;}return 0;}
void phys_free_page(uintptr_t p){if(!p||p%4096||p/4096>=128||!pages[p/4096])abort();pages[p/4096]=0;--allocated;}

int plugin_map_region(plugin_t *pl,uint64_t va,uintptr_t pa,size_t n,unsigned flags)
{(void)n;(void)flags;struct fake *o=lookup(pl);if(!o)return -1;if(va==SESSION_IN_VA)o->input=arena+pa;else if(va==SESSION_OUT_VA)o->output=arena+pa;else return -1;return 0;}
void pm_init(plugin_mgr_t *m,graph_control_t *g){memset(m,0,sizeof(*m));m->gc=g;vfs_init(&m->vfs);}
int pm_set_lifecycle_budget(plugin_mgr_t *m,uint64_t ticks){(void)m;(void)ticks;return 0;}
int pm_register_blob(plugin_mgr_t *m,const char *n,void *b,size_t z){return vfs_add_ramdisk(&m->vfs,n,b,(uint32_t)z);}
void pm_mount_sd(plugin_mgr_t *m,fat_fs_t *f){vfs_mount_sd(&m->vfs,f);}
long pm_load(plugin_mgr_t *m,const char *path)
{
    if(!gc_mutation_allowed(m->gc))return PM_EBUSY;
    pm_slot_t *p=NULL;struct fake *o=NULL;
    for(unsigned i=0;i<PM_MAX_PLUGINS;++i)if(!m->slots[i].used){p=&m->slots[i];break;}
    for(unsigned i=0;i<32;++i)if(!objects[i].pl){o=&objects[i];break;}
    if(!p||!o)return PM_ENOMEM;
    memset(p,0,sizeof(*p));memset(o,0,sizeof(*o));p->pid=next_pid++;p->used=1;
    snprintf(p->path,sizeof(p->path),"%s",path);o->pl=&p->plugin;p->plugin.param_pa=(uintptr_t)o;o->process.pid=p->pid;o->process.state=PROC_READY;
    p->plugin.proc=&o->process;o->gain=strstr(path,"gain")!=NULL;o->value=0x3f000000;
    if(gc_add_plugin(m->gc,p->pid)<0)return PM_ENOMEM;
    return p->pid;
}
plugin_t *pm_plugin(plugin_mgr_t *m,uint32_t pid)
{for(unsigned i=0;i<PM_MAX_PLUGINS;++i)if(m->slots[i].used&&m->slots[i].pid==pid)return &m->slots[i].plugin;return NULL;}
int pm_unload(plugin_mgr_t *m,uint32_t pid)
{
    if(!gc_mutation_allowed(m->gc))return PM_EBUSY;
    for(unsigned i=0;i<PM_MAX_PLUGINS;++i)if(m->slots[i].used&&m->slots[i].pid==pid){
        plugin_t *p=&m->slots[i].plugin;
        if(__atomic_load_n(&p->call_busy,__ATOMIC_ACQUIRE)||__atomic_load_n(&p->host_refs,__ATOMIC_ACQUIRE))abort();
        struct fake *o=lookup(p);if(o->input)phys_free_page((uintptr_t)((unsigned char*)o->input-arena));if(o->output)phys_free_page((uintptr_t)((unsigned char*)o->output-arena));
        audio_graph_remove_node(&m->gc->graph,audio_graph_node_by_pid(&m->gc->graph,pid));
        memset(o,0,sizeof(*o));m->slots[i].used=0;return 0;}
    return PM_ENOENT;
}
long plugin_call_init(plugin_t *p,uint32_t rate,uint32_t n){(void)p;(void)rate;(void)n;return 0;}
long plugin_call_destroy(plugin_t *p){(void)p;return 0;}
long plugin_call_set_param(plugin_t *p,uint32_t id,uint32_t bits)
{if(id==0)lookup(p)->value=bits;return 0;}
long plugin_call_set_param_worker(plugin_t *p,uint32_t id,uint32_t bits){return plugin_call_set_param(p,id,bits);}
long plugin_call_block(plugin_t *p,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint32_t n)
{
    (void)a;(void)b;(void)c;(void)d;struct fake *o=lookup(p);if(!o)abort();
    __atomic_store_n(&p->call_busy,1,__ATOMIC_RELEASE);
    uint32_t *in=o->input,*out=o->output;
    for(unsigned i=0;i<n;++i){
        out[i]=o->gain?sample_float(((int64_t)sample_q31(in[i])*sample_q31(o->value))/2147483648ll):o->value;
        out[n+i]=o->gain?sample_float(((int64_t)sample_q31(in[n+i])*sample_q31(o->value))/2147483648ll):o->value^0x80000000u;
    }
    __atomic_store_n(&p->call_busy,0,__ATOMIC_RELEASE);return 0;
}
void process_kill(process_t *p,long code){p->state=PROC_KILLED;p->exit_code=code;__atomic_store_n(&bad,1,__ATOMIC_RELAXED);}
long budget_call(long (*fn)(void *),void *ctx,uint64_t ticks){(void)ticks;return fn(ctx);}
static uint64_t now(void){return __atomic_load_n(&time_ticks,__ATOMIC_RELAXED);}
static void *worker(void *w)
{while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)){if(!aw_worker_step(w))thrd_yield();}return NULL;}
static void *cadence(void *unused)
{
    (void)unused;int16_t pcm[128];uint64_t seq=0;
    while(!__atomic_load_n(&stop_cadence,__ATOMIC_ACQUIRE)){
        while(!tm_drained(&app.runtime))thrd_yield();
        ++seq;__atomic_store_n(&time_ticks,seq*1000000+1000,__ATOMIC_RELAXED);
        int fresh=session_tick(&app,seq,seq*1000000,pcm);
        if(fresh){
            for(unsigned i=0;i<128;++i)if(pcm[i]!=(i&1?-8192:8192))__atomic_store_n(&bad,1,__ATOMIC_RELAXED);
            __atomic_fetch_add(&rendered,1,__ATOMIC_RELAXED);
        }
        thrd_yield();
    }
    while(!tm_drained(&app.runtime))thrd_yield();
    return NULL;
}
int main(void)
{
    audio_worker_t *w[3];for(unsigned i=0;i<3;++i){aw_init(&workers[i],i+1);w[i]=&workers[i];}
    tm_limits_t limits={{1000000,1000,1000},1000};
    CHECK(session_init(&app,w,&limits,now,1000000,64,64,100000,14)==0);
    app.timeout_ticks=UINT64_MAX/4;
    temporal_contract_t c={.period=1000000,.deadline=950000,.cpu_budget=100000,.criticality=TC_HARD,.overrun_policy=TC_MUTE};
    long source=session_load(&app,"source",&c),gain=session_load(&app,"gain",&c);
    CHECK(source>0&&gain>0);CHECK(session_wire(&app,(uint32_t)source,(uint32_t)gain,0)==0);
    CHECK(session_wire(&app,(uint32_t)gain,0,0)==0);
    CHECK(session_start(&app)==0);
    pthread_t thread[4];for(unsigned i=0;i<3;++i)CHECK(!pthread_create(&thread[i],NULL,worker,w[i]));
    CHECK(!pthread_create(&thread[3],NULL,cadence,NULL));
    for(unsigned cycle=0;cycle<200;++cycle){
        long extra=session_load(&app,"gain",&c);CHECK(extra>0);
        CHECK(session_wire(&app,(uint32_t)source,(uint32_t)extra,0)==0);
        CHECK(session_unwire(&app,(uint32_t)source,(uint32_t)extra)==0);
        CHECK(session_unload(&app,(uint32_t)extra)==0);
        CHECK(session_pin(&app,(uint32_t)source,2)==0);
        CHECK(session_pin(&app,(uint32_t)source,1)==0);
        CHECK(session_set_param(&app,(uint32_t)gain,0,0x3f000000)==0);
        CHECK(session_wire(&app,(uint32_t)gain,(uint32_t)source,0)<0);
        session_preset_t before;CHECK(session_capture(&app,&before)==0&&before.patch.n_plugins==2);
    }
    __atomic_store_n(&stop_cadence,1,__ATOMIC_RELEASE);
    CHECK(!pthread_join(thread[3],NULL));
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<3;++i)CHECK(!pthread_join(thread[i],NULL));
    CHECK(!bad&&rendered>=1000&&app.swaps==1200);
    CHECK(session_pause(&app)==0&&session_clear(&app)==0&&allocated==0);
    printf("SESSION CONCURRENCY: PASS (%u checks; 1200 live swaps; %llu exact PCM frames)\n",checks,(unsigned long long)rendered);
}
