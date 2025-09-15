#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
  for (int i = 0; i < 20; i++) {
    printf("Sleeping %d\n", i);
    sleep(1);
  }
  fork();
  printf("Done\n");
}
