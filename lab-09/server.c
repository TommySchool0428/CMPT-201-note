#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 64
#define PORT 8000
#define LISTEN_BACKLOG 32

#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

// Shared counters for: total # messages, and counter of clients (used for
// assigning client IDs)
int total_message_count = 0;
int client_id_counter = 1;

// Mutexs to protect above global state.
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t client_id_mutex = PTHREAD_MUTEX_INITIALIZER;

struct client_info {
  int cfd;
  int client_id;
};

void *handle_client(void *arg) {
  struct client_info *client = (struct client_info *)arg;

  char buf[BUF_SIZE + 1];
  ssize_t num_read;

  for (;;) {
    num_read = read(client->cfd, buf, BUF_SIZE);
    if (num_read == 0) {
      /* client closed connection */
      break;
    } else if (num_read == -1) {
      perror("read");
      break;
    }

    /* ensure NUL termination for printing */
    if (num_read > 0) {
      if (num_read > BUF_SIZE)
        num_read = BUF_SIZE;
      buf[num_read] = '\0';

      /* update global message count in a thread-safe way */
      int this_msg_no;
      pthread_mutex_lock(&count_mutex);
      total_message_count++;
      this_msg_no = total_message_count;
      pthread_mutex_unlock(&count_mutex);

      printf("Msg # %4d; Client ID %d: %s", this_msg_no,
             client->client_id, buf);
      if (buf[num_read - 1] != '\n')
        printf("\n");
      fflush(stdout);
    }
  }

  printf("Ending thread for client %d\n", client->client_id);
  close(client->cfd);
  free(client);
  return NULL;
}

int main() {
  struct sockaddr_in addr;
  int sfd;

  sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1) {
    handle_error("socket");
  }

  /* allow quick reuse of the port if in TIME_WAIT */
  int optval = 1;
  if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
    handle_error("setsockopt");
  }

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1) {
    handle_error("bind");
  }

  if (listen(sfd, LISTEN_BACKLOG) == -1) {
    handle_error("listen");
  }

  for (;;) {
    int cfd = accept(sfd, NULL, NULL);
    if (cfd == -1) {
      if (errno == EINTR)
        continue;
      perror("accept");
      continue;
    }

    struct client_info *ci = malloc(sizeof(*ci));
    if (!ci) {
      perror("malloc");
      close(cfd);
      continue;
    }
    ci->cfd = cfd;

    pthread_mutex_lock(&client_id_mutex);
    ci->client_id = client_id_counter++;
    pthread_mutex_unlock(&client_id_mutex);

    printf("New client created! ID %d on socket FD %d\n", ci->client_id,
           ci->cfd);

    pthread_t tid;
    if (pthread_create(&tid, NULL, handle_client, ci) != 0) {
      perror("pthread_create");
      close(ci->cfd);
      free(ci);
      continue;
    }
    pthread_detach(tid);
  }

  if (close(sfd) == -1) {
    handle_error("close");
  }

  return 0;
}