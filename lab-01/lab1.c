#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *line = NULL;
  size_t length = 0;

  printf("Please enter some text: ");
  ssize_t text = getline(&line, &length, stdin);
  if (text == -1) {
    perror("getline failed");
    free(line);
    exit(EXIT_FAILURE);
  }

  char *str1, *token;
  char *savedptr;
  token = strtok_r(line, " ", &savedptr);
  printf("Tokens: \n");
  while (token) {
    printf("\t %s\n", token);
    token = strtok_r(NULL, " ", &savedptr);
  }
  free(line);
  exit(EXIT_SUCCESS);
}
