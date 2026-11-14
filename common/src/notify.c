#include "aqm/notify.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int aqm_notify(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un sa;
    size_t plen;
    socklen_t salen;
    ssize_t n;
    int fd, err;

    if (!path || !*path)
        return 0;                       /* started by hand, not by systemd */
    plen = strlen(path);
    if ((path[0] != '/' && path[0] != '@') || plen >= sizeof(sa.sun_path))
        return -EINVAL;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, plen);
    if (path[0] == '@')
        sa.sun_path[0] = '\0';          /* abstract socket namespace */
    salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen);

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    n = sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, salen);
    err = errno;
    close(fd);
    return n < 0 ? -err : 1;
}
