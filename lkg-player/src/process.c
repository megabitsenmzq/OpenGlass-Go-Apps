#include "player.h"

#include <errno.h>
#include <sys/prctl.h>
#include <unistd.h>

int process_bind_parent(pid_t parent_pid)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
        return -1;
    if (parent_pid > 0 && getppid() != parent_pid) {
        errno = ESRCH;
        return -1;
    }
    return 0;
}
