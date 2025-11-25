/* Simple group chat server for A11
 * Usage: ./server <port> <# of clients>
 *
 * Non-blocking server using select() and per-client outgoing buffers
 * to avoid blocking during broadcasts under load.
 */

#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <fcntl.h>

/* FIX: Increased buffer size to 10MB to handle high throughput */
#define OUTBUF_SIZE (10 * 1024 * 1024)

typedef struct client_s {
  int fd;
  struct sockaddr_in addr;
  char buf[2048];
  size_t buflen;
  int alive; /* still participating (hasn't sent type1) */
  int got_type1; /* whether we've already recorded this client's type1 */
  /* outgoing buffer (non-blocking writes) */
  char *outbuf;
  size_t out_head; /* bytes already sent */
  size_t out_tail; /* total bytes enqueued */
} client_t;

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <port> <# of clients>\n", argv[0]);
    return EXIT_FAILURE;
  }

  /* Close any inherited file descriptors to avoid FD exhaustion/pollution.
   * The environment seems to be leaking thousands of FDs (auxv handles).
   * We close from 3 up to a reasonable limit.
   * FIX: Added this loop to clean up leaked FDs from the environment.
   */
  for (int fd = 3; fd < 4096; fd++) {
      close(fd);
  }

  int port = atoi(argv[1]);
  int expected_clients = atoi(argv[2]);

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) { perror("socket"); return 1; }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in srv;
  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_addr.s_addr = INADDR_ANY;
  srv.sin_port = htons(port);

  if (bind(listen_fd, (struct sockaddr*)&srv, sizeof(srv)) < 0) { perror("bind"); return 1; }
  if (listen(listen_fd, 128) < 0) { perror("listen"); return 1; }

  /* make listen non-blocking */
  int lflags = fcntl(listen_fd, F_GETFL, 0);
  if (lflags >= 0) fcntl(listen_fd, F_SETFL, lflags | O_NONBLOCK);

  static client_t clients[FD_SETSIZE];
  for (int i = 0; i < FD_SETSIZE; i++) {
    clients[i].fd = -1;
    clients[i].buflen = 0;
    clients[i].alive = 0;
    clients[i].got_type1 = 0;
    clients[i].out_head = clients[i].out_tail = 0;
    clients[i].outbuf = NULL;
  }

  int num_clients = 0;
  int type1_count = 0;
  time_t first_type1_time = 0;

  fd_set master_read, master_write, readfds, writefds;
  FD_ZERO(&master_read);
  FD_ZERO(&master_write);
  FD_SET(listen_fd, &master_read);
  int fdmax = listen_fd;

  while (1) {
    readfds = master_read;
    writefds = master_write;
    
    /* Use a short timeout so we can check grace period */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; /* 100ms */
    
    int sel = select(fdmax + 1, &readfds, &writefds, NULL, &tv);
    if (sel < 0) {
      if (errno == EINTR) continue;
      perror("select");
      break;
    }
    
    /* Check grace timeout outside of any client loop */
    if (first_type1_time != 0) {
      time_t now = time(NULL);
      time_t elapsed = now - first_type1_time;
      /* After receiving ANY type1, wait only briefly then broadcast */
      if (type1_count < expected_clients && elapsed >= 1) {
        fprintf(stderr, "[server] Grace timeout: received %d/%d type1s, broadcasting shutdown\n", 
                type1_count, expected_clients);
        char tout[2] = {1, '\n'};
        int enqueued = 0;
        for (int j = 0; j < FD_SETSIZE; j++) {
          if (clients[j].fd != -1) {
            size_t avail = OUTBUF_SIZE - clients[j].out_tail;
            if (clients[j].out_head > 0) {
              size_t rem = clients[j].out_tail - clients[j].out_head;
              memmove(clients[j].outbuf, clients[j].outbuf + clients[j].out_head, rem);
              clients[j].out_tail = rem;
              clients[j].out_head = 0;
              avail = OUTBUF_SIZE - clients[j].out_tail;
            }
            if (avail >= 2) {
              memcpy(clients[j].outbuf + clients[j].out_tail, tout, 2);
              clients[j].out_tail += 2;
              FD_SET(clients[j].fd, &master_write);
              if (clients[j].fd > fdmax) fdmax = clients[j].fd;
              enqueued++;
            }
          }
        }
        fprintf(stderr, "[server] Grace broadcast type1 (forced) got %d expected %d\n", type1_count, expected_clients);

        int finishing = 1;
        while (finishing) {
          fd_set wfds = master_write;
          struct timeval flush_tv = {1, 0};
          if (select(fdmax + 1, NULL, &wfds, NULL, &flush_tv) <= 0) break;
          finishing = 0;
          for (int k = 0; k < FD_SETSIZE; k++) {
            if (clients[k].fd == -1) continue;
            int wfd = clients[k].fd;
            if (FD_ISSET(wfd, &wfds)) {
              size_t tosend = clients[k].out_tail - clients[k].out_head;
              if (tosend > 0) {
                ssize_t s = send(wfd, clients[k].outbuf + clients[k].out_head, tosend, 0);
                if (s > 0) {
                  clients[k].out_head += s;
                  if (clients[k].out_head == clients[k].out_tail) {
                    clients[k].out_head = clients[k].out_tail = 0;
                    FD_CLR(wfd, &master_write);
                  } else {
                    finishing = 1;
                  }
                } else if (s < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                  close(wfd);
                  FD_CLR(wfd, &master_read);
                  FD_CLR(wfd, &master_write);
                  if (clients[k].outbuf) { free(clients[k].outbuf); clients[k].outbuf = NULL; }
                  clients[k].fd = -1;
                  clients[k].alive = 0;
                } else {
                  finishing = 1;
                }
              }
            }
          }
        }

        close(listen_fd);
        for (int j = 0; j < FD_SETSIZE; j++) {
            if (clients[j].fd != -1) close(clients[j].fd);
            if (clients[j].outbuf) { free(clients[j].outbuf); clients[j].outbuf = NULL; }
        }
        return 0;
      }
    }
    
    if (sel == 0) continue; /* timeout, loop back */

    /* Flush writable clients first */
    for (int i = 0; i < FD_SETSIZE; i++) {
      if (clients[i].fd == -1) continue;
      int wfd = clients[i].fd;
      if (FD_ISSET(wfd, &writefds)) {
        size_t tosend = clients[i].out_tail - clients[i].out_head;
        if (tosend > 0) {
          ssize_t s = send(wfd, clients[i].outbuf + clients[i].out_head, tosend, 0);
          if (s > 0) {
            clients[i].out_head += s;
            if (clients[i].out_head == clients[i].out_tail) {
              clients[i].out_head = clients[i].out_tail = 0;
              FD_CLR(wfd, &master_write);
            }
          } else if (s < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(wfd);
            FD_CLR(wfd, &master_read);
            FD_CLR(wfd, &master_write);
            if (clients[i].outbuf) { free(clients[i].outbuf); clients[i].outbuf = NULL; }
            clients[i].fd = -1;
            clients[i].alive = 0;
            num_clients--;
          }
        } else {
          FD_CLR(wfd, &master_write);
        }
      }
    }

    /* Accept new connections */
    if (FD_ISSET(listen_fd, &readfds)) {
      while (1) {
        struct sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);
        int cfd = accept(listen_fd, (struct sockaddr *)&cli_addr, &len);
        if (cfd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          if (errno == EINTR) continue;
          perror("accept");
          break;
        }
        int slot = -1;
        for (int k = 0; k < FD_SETSIZE; k++) if (clients[k].fd == -1) { slot = k; break; }
        if (slot == -1) { close(cfd); continue; }
        if (cfd >= FD_SETSIZE) {
            /* FIX: Added check to prevent buffer overflow in FD_SET if FD is too high */
            fprintf(stderr, "Error: fd %d >= FD_SETSIZE %d\n", cfd, FD_SETSIZE);
            close(cfd);
            continue;
        }
        int flags = fcntl(cfd, F_GETFL, 0);
        if (flags >= 0) fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
        
        /* FIX: Use dynamic buffer instead of stack to handle large backpressure */
        clients[slot].outbuf = malloc(OUTBUF_SIZE);
        if (!clients[slot].outbuf) {
            close(cfd);
            continue;
        }

        clients[slot].fd = cfd;
        clients[slot].addr = cli_addr;
        clients[slot].buflen = 0;
        clients[slot].alive = 1;
        clients[slot].got_type1 = 0;
        clients[slot].out_head = clients[slot].out_tail = 0;
        FD_SET(cfd, &master_read);
        if (cfd > fdmax) fdmax = cfd;
        num_clients++;
      }
    }

    /* Handle readable clients */
    for (int i = 0; i < FD_SETSIZE; i++) {
      if (clients[i].fd == -1) continue;
      int fd = clients[i].fd;
      if (!FD_ISSET(fd, &readfds)) continue;

      size_t space = sizeof(clients[i].buf) - clients[i].buflen - 1;
      if (space == 0) {
        close(fd);
        FD_CLR(fd, &master_read);
        FD_CLR(fd, &master_write);
        if (clients[i].outbuf) { free(clients[i].outbuf); clients[i].outbuf = NULL; }
        clients[i].fd = -1;
        clients[i].alive = 0;
        num_clients--;
        continue;
      }
      ssize_t r = recv(fd, clients[i].buf + clients[i].buflen, space, 0);
      int peer_closed = 0;
      if (r < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        close(fd);
        FD_CLR(fd, &master_read);
        FD_CLR(fd, &master_write);
        if (clients[i].outbuf) { free(clients[i].outbuf); clients[i].outbuf = NULL; }
        clients[i].fd = -1;
        clients[i].alive = 0;
        num_clients--;
        continue;
      } else if (r == 0) {
        peer_closed = 1;
      } else {
        clients[i].buflen += r;
        clients[i].buf[clients[i].buflen] = '\0';
      }

      size_t processed = 0;
      for (size_t p = 0; p < clients[i].buflen; p++) {
        if (clients[i].buf[p] == '\n') {
          size_t msglen = p - processed + 1;
          if (msglen >= 1) {
            uint8_t type = clients[i].buf[processed];
            if (type == 0) {
              char out[MAX_MSG_SIZE + 1 + 4 + 2];
              size_t payload_len = msglen - 1;
              if (payload_len > MAX_MSG_SIZE) payload_len = MAX_MSG_SIZE;
              out[0] = 0;
              uint32_t ip_n = clients[i].addr.sin_addr.s_addr;
              uint16_t port_n = clients[i].addr.sin_port;
              memcpy(out + 1, &ip_n, 4);
              memcpy(out + 1 + 4, &port_n, 2);
              memcpy(out + 1 + 4 + 2, clients[i].buf + processed + 1, payload_len);
              size_t out_len = 1 + 4 + 2 + payload_len;

              /* Enqueue to all alive clients' outbufs (non-blocking) */
              for (int j = 0; j < FD_SETSIZE; j++) {
                if (clients[j].fd != -1 && clients[j].alive) {
                  size_t avail = OUTBUF_SIZE - clients[j].out_tail;
                  if (avail < out_len && clients[j].out_head > 0) {
                    size_t rem = clients[j].out_tail - clients[j].out_head;
                    memmove(clients[j].outbuf, clients[j].outbuf + clients[j].out_head, rem);
                    clients[j].out_tail = rem;
                    clients[j].out_head = 0;
                    avail = OUTBUF_SIZE - clients[j].out_tail;
                  }
                  if (avail >= out_len) {
                    memcpy(clients[j].outbuf + clients[j].out_tail, out, out_len);
                    clients[j].out_tail += out_len;
                    FD_SET(clients[j].fd, &master_write);
                    if (clients[j].fd > fdmax) fdmax = clients[j].fd;
                  } else {
                    /* If we can't enqueue, drop the client to avoid blocking */
                    close(clients[j].fd);
                    FD_CLR(clients[j].fd, &master_read);
                    FD_CLR(clients[j].fd, &master_write);
                    if (clients[j].outbuf) { free(clients[j].outbuf); clients[j].outbuf = NULL; }
                    clients[j].fd = -1;
                    clients[j].alive = 0;
                    num_clients--;
                  }
                }
              }
            } else if (type == 1) {
              if (!clients[i].got_type1) {
                type1_count++;
                clients[i].got_type1 = 1;
                /* Keep alive=1 so they still receive broadcasts until final shutdown */
                char ipbuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &clients[i].addr.sin_addr, ipbuf, sizeof(ipbuf));
                fprintf(stderr, "[server] Received type1 from %s:%u -> count=%d/%d\n",
                        ipbuf, ntohs(clients[i].addr.sin_port), type1_count, expected_clients);
                if (first_type1_time == 0) first_type1_time = time(NULL);
              }

              if (type1_count >= expected_clients) {
                /* Enqueue final type1 to all and flush before exit */
                char tout[2] = {1, '\n'};
                int enqueued = 0;
                for (int j = 0; j < FD_SETSIZE; j++) {
                  if (clients[j].fd != -1) {
                    size_t avail = OUTBUF_SIZE - clients[j].out_tail;
                    if (clients[j].out_head > 0) {
                      size_t rem = clients[j].out_tail - clients[j].out_head;
                      memmove(clients[j].outbuf, clients[j].outbuf + clients[j].out_head, rem);
                      clients[j].out_tail = rem;
                      clients[j].out_head = 0;
                      avail = OUTBUF_SIZE - clients[j].out_tail;
                    }
                    if (avail >= 2) {
                      memcpy(clients[j].outbuf + clients[j].out_tail, tout, 2);
                      clients[j].out_tail += 2;
                      FD_SET(clients[j].fd, &master_write);
                      if (clients[j].fd > fdmax) fdmax = clients[j].fd;
                      enqueued++;
                    }
                  }
                }
                fprintf(stderr, "[server] Broadcasting type1 to %d clients (expected %d)\n", enqueued, expected_clients);

                /* Flush writes until buffers empty or sockets closed */
                int finishing = 1;
                while (finishing) {
                  fd_set rfds, wfds;
                  FD_ZERO(&rfds);
                  wfds = master_write;
                  struct timeval flush_tv = {1, 0};
                  if (select(fdmax + 1, &rfds, &wfds, NULL, &flush_tv) <= 0) break;
                  finishing = 0;
                  for (int k = 0; k < FD_SETSIZE; k++) {
                    if (clients[k].fd == -1) continue;
                    int wfd = clients[k].fd;
                    if (FD_ISSET(wfd, &wfds)) {
                      size_t tosend = clients[k].out_tail - clients[k].out_head;
                      if (tosend > 0) {
                        ssize_t s = send(wfd, clients[k].outbuf + clients[k].out_head, tosend, 0);
                        if (s > 0) {
                          clients[k].out_head += s;
                          if (clients[k].out_head == clients[k].out_tail) {
                            clients[k].out_head = clients[k].out_tail = 0;
                            FD_CLR(wfd, &master_write);
                          } else {
                            finishing = 1;
                          }
                        } else if (s < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                          close(wfd);
                          FD_CLR(wfd, &master_read);
                          FD_CLR(wfd, &master_write);
                          if (clients[k].outbuf) { free(clients[k].outbuf); clients[k].outbuf = NULL; }
                          clients[k].fd = -1;
                          clients[k].alive = 0;
                        } else {
                          finishing = 1;
                        }
                      }
                    }
                  }
                }

                close(listen_fd);
                for (int j = 0; j < FD_SETSIZE; j++) if (clients[j].fd != -1) close(clients[j].fd);
                return 0;
              }
            }
          }
          processed = p + 1;
        }
      }

      if (processed > 0) {
        size_t left = clients[i].buflen - processed;
        memmove(clients[i].buf, clients[i].buf + processed, left);
        clients[i].buflen = left;
        clients[i].buf[clients[i].buflen] = '\0';
      }

      if (peer_closed) {
        close(fd);
        FD_CLR(fd, &master_read);
        FD_CLR(fd, &master_write);
        if (clients[i].outbuf) { free(clients[i].outbuf); clients[i].outbuf = NULL; }
        clients[i].fd = -1;
        clients[i].alive = 0;
        num_clients--;
      }
    }
  }

  return 0;
}
