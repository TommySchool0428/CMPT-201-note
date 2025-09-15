#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
  printf("START ID=%d, parant PID=%d\n", getpid(), getppid());
  pid_t pid = fork();
  if (pid == 0) {
    printf("CHILD: PID=%d, parent PID=%d\n", getpid(), getppid());
  } else {
    printf("PARENT: PID=%d, child PID=%d\n", getpid(), pid);
  }
}
