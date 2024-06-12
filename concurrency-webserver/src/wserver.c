#include "io_helper.h"
#include "request.h"
#include <bits/pthreadtypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>

char default_root[] = ".";
bool default_shedalg = 1;
char default_threads_count = 1;
char buffers_count = 1;
pthread_cond_t request_cond;
pthread_cond_t request_handled_cond;
pthread_mutex_t buffer_lock;
pthread_mutex_t request_cond_lock;
pthread_mutex_t request_handled_cond_lock;
static bool keepRunning = true;
static int listen_fd;

typedef struct Connection_Manager {
  int *connections;
  ssize_t max_size;
  ssize_t current_size;
  bool shedalg; // 1=FIFO;0=SFF;
} Connection_Manager;

Connection_Manager *Connection_Manager_init(ssize_t size, bool shedalg) {
  Connection_Manager *self =
      (Connection_Manager *)malloc(sizeof(Connection_Manager));
  self->max_size = size;
  self->current_size = 0;
  self->shedalg = shedalg;
  self->connections = (int *)malloc(self->max_size * sizeof(int));

  return self;
}

void Connection_Manager_destroy(Connection_Manager self) {
  free(self.connections);
}

void Connection_Manager_push(Connection_Manager *self, int new_conn) {

  if (self->shedalg) {
    if (self->max_size != self->current_size) {
      if (pthread_mutex_lock(&buffer_lock) != 0) {
        perror("pthread_mutex_lock() error");
        exit(1);
      }

      self->connections[self->current_size] = new_conn;
      self->current_size++;

      if (pthread_cond_signal(&request_cond) != 0) {
        perror("pthread_cond_signal() error");
        exit(1);
      }

      if (pthread_mutex_unlock(&buffer_lock) != 0) {
        perror("pthread_mutex_unlock() error");
        exit(1);
      }

    } else {

      if (pthread_mutex_lock(&request_handled_cond_lock) != 0) {
        perror("pthread_mutex_lock() error");
        exit(1);
      }

      if (pthread_cond_wait(&request_handled_cond,
                            &request_handled_cond_lock) != 0) {
        perror("pthread_cond_wait() error");
        exit(1);
      }

      if (pthread_mutex_lock(&buffer_lock) != 0) {
        perror("pthread_mutex_lock() error");
        exit(1);
      }

      self->connections[self->current_size] = new_conn;
      self->current_size++;

      if (pthread_cond_signal(&request_cond) != 0) {
        perror("pthread_cond_signal() error");
        exit(1);
      }

      if (pthread_mutex_unlock(&buffer_lock) != 0) {
        perror("pthread_mutex_unlock() error");
        exit(1);
      }

      if (pthread_mutex_unlock(&request_handled_cond_lock) != 0) {
        perror("pthread_mutex_unlock() error");
        exit(1);
      }
    }

  } else {
  }
}

void Connection_Manager_sort(Connection_Manager *self) {
  for (ssize_t i = 0; i < self->max_size - 1; i++) {
    self->connections[i] = self->connections[i + 1];
    self->connections[i + 1] = 0;
  }
}

int Connection_Manager_pop(Connection_Manager *self) {
  int conn = 0;

  if (self->shedalg) {
    if (self->current_size != 0) {
      if (pthread_mutex_lock(&buffer_lock) != 0) {
        perror("pthread_mutex_lock() error");
        exit(1);
      }

      conn = self->connections[0];
      self->current_size--;
      Connection_Manager_sort(self);

      if (pthread_cond_signal(&request_handled_cond) != 0) {
        perror("pthread_cond_signal() error");
        exit(1);
      }

      if (pthread_mutex_unlock(&buffer_lock) != 0) {
        perror("pthread_mutex_unlock() error");
        exit(1);
      }
    } else {

      if (pthread_mutex_lock(&request_cond_lock) != 0) {
        perror("pthread_mutex_lock() error");
        exit(1);
      }

      if (pthread_cond_wait(&request_cond, &request_cond_lock) != 0) {
        perror("pthread_cond_wait() error");
        exit(1);
      }

      if (pthread_mutex_lock(&buffer_lock) != 0) {
        perror("pthread_mutex_lock() error");
        exit(1);
      }

      conn = self->connections[0];
      self->current_size--;
      Connection_Manager_sort(self);

      if (pthread_cond_signal(&request_handled_cond) != 0) {
        perror("pthread_cond_signal() error");
        exit(1);
      }

      if (pthread_mutex_unlock(&buffer_lock) != 0) {
        perror("pthread_mutex_unlock() error");
        exit(1);
      }
      if (pthread_mutex_unlock(&request_cond_lock) != 0) {
        perror("pthread_mutex_unlock() error");
        exit(1);
      }
    }
  } else {
  }

  return conn;
}

void int_handler(int sig) {
  keepRunning = false;
  close(listen_fd);
  pthread_cond_broadcast(&request_cond);
  pthread_cond_broadcast(&request_handled_cond);
}

void *do_the_work(void *conn_man) {
  while (keepRunning) {
    // Handle message
    Connection_Manager *connections = (Connection_Manager *)conn_man;
    int request = Connection_Manager_pop(connections);
    request_handle(request);
    close_or_die(request);
  }

  return NULL;
}

//
// ./wserver [-d <basedir>] [-p <portnum>]
//
int main(int argc, char *argv[]) {
  int c;
  char *root_dir = default_root;
  bool shedalg = default_shedalg;
  char threads_count = default_threads_count;

  int port = 10000;
  signal(SIGINT, int_handler);

  while ((c = getopt(argc, argv, "d:p:t:b:s:")) != -1)
    switch (c) {
    case 'd':
      root_dir = optarg;
      break;
    case 'p':
      port = atoi(optarg);
      break;
    case 't':
      threads_count = atoi(optarg);
      break;
    case 'b':
      buffers_count = atoi(optarg);
      break;
    case 's':
      shedalg = atoi(optarg);
      break;
    default:
      fprintf(stderr, "usage: wserver [-d basedir] [-p port]\n");
      exit(1);
    }

  // run out of this directory
  chdir_or_die(root_dir);

  // create request buffer
  Connection_Manager *conn_man =
      Connection_Manager_init(buffers_count, shedalg);

  if (pthread_mutex_init(&buffer_lock, NULL) != 0 ||
      pthread_mutex_init(&request_cond_lock, NULL) != 0 ||
      pthread_mutex_init(&request_handled_cond_lock, NULL) != 0) {
    perror("pthread_mutex_init() error");
    exit(1);
  }

  if (pthread_cond_init(&request_cond, NULL) != 0 ||
      pthread_cond_init(&request_handled_cond, NULL) != 0) {
    perror("pthread_cond_init() error");
    exit(1);
  }

  // create threads pool
  pthread_t thread_pool[threads_count];
  for (ssize_t i = 0; i < threads_count; i++) {
    if (pthread_create(&thread_pool[i], NULL, do_the_work, conn_man) != 0) {
      perror("pthread_create() error");
      exit(1);
    }
  }

  // now, get to work
  listen_fd = open_listen_fd_or_die(port);
  while (keepRunning) {
    struct sockaddr_in client_addr;
    int client_len = sizeof(client_addr);
    int conn_fd = accept_or_die(listen_fd, (sockaddr_t *)&client_addr,
                                (socklen_t *)&client_len);

    if (conn_fd != 0) {
      Connection_Manager_push(conn_man, conn_fd);
    }
  }

  // join threads
  for (ssize_t i = 0; i < threads_count; i++) {
    if (pthread_cond_signal(&request_cond) != 0) {
      perror("pthread_cond_signal() error");
      exit(1);
    }
    pthread_join(thread_pool[i], NULL);
  }

  return 0;
}
