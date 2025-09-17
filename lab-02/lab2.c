#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  char *line = NULL;
  size_t n = 0;

  while (1) {
    printf("Please enter a command: \n");
    ssize_t len = getline(&line, &n, stdin);
    if (len == -1) {
      perror("Failed to read line\n");
      break;
    } else if (len > 0) {
      line[len - 1] = '\0';
    }

    pid_t pid = fork();
    if (pid) {
      int status = 0;

      if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed");
        exit(EXIT_FAILURE);
      }

      if (WIFEXITED(status)) {
        printf("Child exited. \n");
      }

      free(line);
      line = NULL;
    } else {
      if (execl(line, line, (char *)NULL) == -1) {
        perror("execl failed \n");
        exit(EXIT_FAILURE);
      }
    }
  }
}
