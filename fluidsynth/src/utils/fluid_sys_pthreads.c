/* Minimal pthreads-based OSAL for embedded FluidSynth (replaces cpp11) */
#include "fluid_sys.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Mutex */
typedef pthread_mutex_t fluid_mutex_impl_t;
typedef pthread_mutex_t fluid_rec_mutex_impl_t;

void _fluid_mutex_init(fluid_mutex_t *mutex) {
    *mutex = malloc(sizeof(fluid_mutex_impl_t));
    pthread_mutex_init((fluid_mutex_impl_t *)*mutex, NULL);
}
void fluid_mutex_destroy(fluid_mutex_t mutex) {
    pthread_mutex_destroy((fluid_mutex_impl_t *)mutex);
    free(mutex);
}
void _fluid_mutex_lock(fluid_mutex_t *mutex) {
    *mutex = malloc(sizeof(fluid_mutex_impl_t));
    pthread_mutex_init((fluid_mutex_impl_t *)*mutex, NULL);
    pthread_mutex_lock((fluid_mutex_impl_t *)*mutex);
}
void fluid_mutex_unlock(fluid_mutex_t mutex) {
    pthread_mutex_unlock((fluid_mutex_impl_t *)mutex);
}

/* Recursive mutex */
void _fluid_rec_mutex_init(fluid_rec_mutex_t *mutex) {
    *mutex = malloc(sizeof(fluid_rec_mutex_impl_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init((fluid_rec_mutex_impl_t *)*mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
void fluid_rec_mutex_destroy(fluid_rec_mutex_t mutex) {
    pthread_mutex_destroy((fluid_rec_mutex_impl_t *)mutex);
    free(mutex);
}
void fluid_rec_mutex_lock(fluid_rec_mutex_t mutex) {
    pthread_mutex_lock((fluid_rec_mutex_impl_t *)mutex);
}
void fluid_rec_mutex_unlock(fluid_rec_mutex_t mutex) {
    pthread_mutex_unlock((fluid_rec_mutex_impl_t *)mutex);
}

/* Condition variable */
typedef struct { pthread_cond_t cond; pthread_mutex_t mutex; } fluid_cond_impl_t;
typedef fluid_cond_impl_t fluid_cond_mutex_impl_t;

fluid_cond_mutex_t *new_fluid_cond_mutex(void) {
    fluid_cond_mutex_impl_t *m = malloc(sizeof(fluid_cond_mutex_impl_t));
    pthread_mutex_init(&m->mutex, NULL);
    pthread_cond_init(&m->cond, NULL);
    return (fluid_cond_mutex_t *)m;
}
void delete_fluid_cond_mutex(fluid_cond_mutex_t *mutex) {
    fluid_cond_mutex_impl_t *m = (fluid_cond_mutex_impl_t *)mutex;
    pthread_mutex_destroy(&m->mutex);
    pthread_cond_destroy(&m->cond);
    free(m);
}
void fluid_cond_mutex_lock(fluid_cond_mutex_t *mutex) {
    pthread_mutex_lock(&((fluid_cond_mutex_impl_t *)mutex)->mutex);
}
void fluid_cond_mutex_unlock(fluid_cond_mutex_t *mutex) {
    pthread_mutex_unlock(&((fluid_cond_mutex_impl_t *)mutex)->mutex);
}

typedef fluid_cond_impl_t fluid_cond_impl_t;
fluid_cond_t new_fluid_cond(void) {
    fluid_cond_impl_t *c = malloc(sizeof(fluid_cond_impl_t));
    pthread_cond_init(&c->cond, NULL);
    pthread_mutex_init(&c->mutex, NULL);
    return (fluid_cond_t)c;
}
void delete_fluid_cond(fluid_cond_t cond) {
    fluid_cond_impl_t *c = (fluid_cond_impl_t *)cond;
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}
void fluid_cond_signal(fluid_cond_t cond) {
    pthread_cond_signal(&((fluid_cond_impl_t *)cond)->cond);
}
void fluid_cond_broadcast(fluid_cond_t cond) {
    pthread_cond_broadcast(&((fluid_cond_impl_t *)cond)->cond);
}
void fluid_cond_wait(fluid_cond_t cond, fluid_cond_mutex_t *mutex) {
    pthread_cond_wait(&((fluid_cond_impl_t *)cond)->cond,
                      &((fluid_cond_mutex_impl_t *)mutex)->mutex);
}
void fluid_cond_mutex_unlock(fluid_cond_mutex_t *mutex) {
    pthread_mutex_unlock(&((fluid_cond_mutex_impl_t *)mutex)->mutex);
}

/* Thread */
typedef struct { pthread_t id; void *(*func)(void *); void *data; } fluid_thread_impl_t;

fluid_thread_t *new_fluid_thread(const char *name, fluid_thread_func_t func, void *data, int prio, int detach) {
    fluid_thread_impl_t *t = malloc(sizeof(fluid_thread_impl_t));
    t->func = (void *(*)(void *))func;
    t->data = data;
    pthread_create(&t->id, NULL, (void *(*)(void *))func, data);
    if (detach) pthread_detach(t->id);
    return (fluid_thread_t *)t;
}
void delete_fluid_thread(fluid_thread_t *_thread) {
    fluid_thread_impl_t *t = (fluid_thread_impl_t *)_thread;
    pthread_join(t->id, NULL);
    free(t);
}
int fluid_thread_join(fluid_thread_t *_thread) {
    fluid_thread_impl_t *t = (fluid_thread_impl_t *)_thread;
    return pthread_join(t->id, NULL) == 0 ? FLUID_OK : FLUID_FAILED;
}

/* Time */
void fluid_msleep(unsigned int msecs) {
    usleep(msecs * 1000);
}
double fluid_utime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* File stat */
int fluid_stat(const char *path, fluid_stat_buf_t *buffer) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    buffer->st_mtime = st.st_mtime;
    return 0;
}

/* Private data */
typedef struct { void *value; } fluid_private_impl_t;
void _fluid_private_init(fluid_private_t *priv) {
    *priv = malloc(sizeof(fluid_private_impl_t));
    ((fluid_private_impl_t *)*priv)->value = NULL;
}
void fluid_private_free(fluid_private_t priv) { free(priv); }
void *fluid_private_get(fluid_private_t priv) {
    return ((fluid_private_impl_t *)priv)->value;
}
void fluid_private_set(fluid_private_t priv, void *value) {
    ((fluid_private_impl_t *)priv)->value = value;
}

/* Atomic lock (global) */
static pthread_mutex_t atomic_lock_pthread = PTHREAD_MUTEX_INITIALIZER;
fluid_mutex_t _atomic_lock = &atomic_lock_pthread;
