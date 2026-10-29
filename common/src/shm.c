#include "aqm/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static aqm_shm_t *map_fd(int fd)
{
    void *p = mmap(NULL, sizeof(aqm_shm_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

aqm_shm_t *aqm_shm_create(void)
{
    aqm_shm_t *p;
    int fd, i;

    shm_unlink(AQM_SHM_NAME);                     /* remove stale segment */
    fd = shm_open(AQM_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, sizeof(aqm_shm_t)) != 0) {
        close(fd);
        return NULL;
    }
    p = map_fd(fd);
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    for (i = 0; i < AQM_RING_COUNT; i++) {
        if (aqm_ring_init(&p->ring[i]) != 0) {
            munmap(p, sizeof(*p));
            return NULL;
        }
    }
    p->version = 1;
    p->running = 1;
    __atomic_store_n(&p->magic, AQM_SHM_MAGIC, __ATOMIC_RELEASE);
    return p;
}

aqm_shm_t *aqm_shm_attach(void)
{
    aqm_shm_t *p;
    int fd = shm_open(AQM_SHM_NAME, O_RDWR, 0);

    if (fd < 0)
        return NULL;
    p = map_fd(fd);
    if (!p)
        return NULL;
    if (__atomic_load_n(&p->magic, __ATOMIC_ACQUIRE) != AQM_SHM_MAGIC) {
        munmap(p, sizeof(*p));
        errno = EAGAIN;                           /* owner not ready yet */
        return NULL;
    }
    return p;
}

void aqm_shm_detach(aqm_shm_t *p)
{
    if (p)
        munmap(p, sizeof(*p));
}

int aqm_shm_destroy(void)
{
    return shm_unlink(AQM_SHM_NAME);
}
