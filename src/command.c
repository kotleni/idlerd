#include "command.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

void run_command(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("sh", "sh", "-c", cmd, NULL);
        fprintf(stderr, "[error] failed to exec `%s`\n", cmd);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[warn] command exited with %d: %s\n",
                    WEXITSTATUS(status), cmd);
        }
    }
}
