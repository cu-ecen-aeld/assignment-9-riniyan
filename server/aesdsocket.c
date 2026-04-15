#define _GNU_SOURCE
#include "../aesd-char-driver/aesd_ioctl.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PORT 9000
#define AESD_CHAR_DEVICE "/dev/aesdchar"
#define BACKLOG 10
#define RECV_CHUNK 1024

static volatile sig_atomic_t g_exit_requested = 0;
static int g_server_fd = -1;
// Added code
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
struct thread_node {
  pthread_t thread_id;
  int client_fd;
  bool thread_complete;
  SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list, thread_node);
static struct thread_list g_threads = SLIST_HEAD_INITIALIZER(g_threads);

static void handle_signal(int signo) {
  (void)signo;
  g_exit_requested = 1;
}

static int send_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  size_t total = 0;
  while (total < len) {
    ssize_t n = send(fd, p + total, len - total, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    total += (size_t)n;
  }
  return 0;
}

static int daemonize(void) {

  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid > 0)
    exit(0); // parent exits

  if (setsid() < 0)
    return -1;

  pid = fork();
  if (pid < 0)
    return -1;
  if (pid > 0)
    exit(0); // first child exits

  umask(0);
  if (chdir("/") < 0)
    return -1;

  // Redirect stdio to /dev/null
  int fd = open("/dev/null", O_RDWR);
  if (fd < 0)
    return -1;
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd > 2)
    close(fd);

  return 0;
}

static void *client_thread_func(void *arg) {
  struct thread_node *node = (struct thread_node *)arg;
  int client_fd = node->client_fd;

  char recvbuf[RECV_CHUNK];
  char *line = NULL;
  size_t line_cap = 0;
  size_t line_len = 0;

  while (!g_exit_requested) {
    ssize_t n = recv(client_fd, recvbuf, sizeof(recvbuf), 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      syslog(LOG_ERR, "recv failed: %s", strerror(errno));
      break;
    }

    if (n == 0)
      break;

    // grow buffer
    if (line_len + (size_t)n + 1 > line_cap) {
      size_t newcap = (line_cap == 0) ? 2048 : line_cap;
      while (newcap < line_len + (size_t)n + 1)
        newcap *= 2;
      char *tmp = realloc(line, newcap);
      if (!tmp) {
        syslog(LOG_ERR, "realloc failed");
        break;
      }
      line = tmp;
      line_cap = newcap;
    }

    memcpy(line + line_len, recvbuf, (size_t)n);
    line_len += (size_t)n;
    line[line_len] = '\0';

    // process each newline-terminated record
    char *start = line;
    char *nl;
    while ((nl = memchr(start, '\n', (line + line_len) - start)) != NULL) {
      size_t record_len = (size_t)(nl - start) + 1;

      // null terminate just this record for string comparison
      char saved = *(nl + 1);
      *(nl + 1) = '\0';

      pthread_mutex_lock(&g_file_mutex);

      // Check if this is an ioctl seek command
      if (strncmp(start, "AESDCHAR_IOCSEEKTO:", 19) == 0) {
        // Parse X and Y from "AESDCHAR_IOCSEEKTO:X,Y\n"
        unsigned int write_cmd = 0;
        unsigned int write_cmd_offset = 0;
        if (sscanf(start + 19, "%u,%u", &write_cmd, &write_cmd_offset) == 2) {
          // Open device, send ioctl, read back, send to client
          int dev_fd = open(AESD_CHAR_DEVICE, O_RDWR);
          if (dev_fd < 0) {
            syslog(LOG_ERR, "open(%s) failed: %s", AESD_CHAR_DEVICE,
                   strerror(errno));
          } else {
            struct aesd_seekto seekto;
            seekto.write_cmd = write_cmd;
            seekto.write_cmd_offset = write_cmd_offset;

            if (ioctl(dev_fd, AESDCHAR_IOCSEEKTO, &seekto) != 0) {
              syslog(LOG_ERR, "ioctl failed: %s", strerror(errno));
            } else {
              // Read from current position (set by ioctl) and send back
              char filebuf[RECV_CHUNK];
              for (;;) {
                ssize_t r = read(dev_fd, filebuf, sizeof(filebuf));
                if (r < 0) {
                  if (errno == EINTR)
                    continue;
                  break;
                }
                if (r == 0)
                  break;
                if (send_all(client_fd, filebuf, (size_t)r) != 0)
                  break;
              }
            }
            close(dev_fd);
          }
        }
      } else {
        // Normal write: open device, write record, seek to start, read all,
        // send back
        int dev_fd = open(AESD_CHAR_DEVICE, O_RDWR);
        if (dev_fd < 0) {
          syslog(LOG_ERR, "open(%s) failed: %s", AESD_CHAR_DEVICE,
                 strerror(errno));
        } else {
          // Write the record
          size_t off = 0;
          while (off < record_len) {
            ssize_t w = write(dev_fd, start + off, record_len - off);
            if (w < 0) {
              if (errno == EINTR)
                continue;
              syslog(LOG_ERR, "write failed: %s", strerror(errno));
              break;
            }
            off += (size_t)w;
          }

          // Seek back to start and read everything back
          if (lseek(dev_fd, 0, SEEK_SET) == 0) {
            char filebuf[RECV_CHUNK];
            for (;;) {
              ssize_t r = read(dev_fd, filebuf, sizeof(filebuf));
              if (r < 0) {
                if (errno == EINTR)
                  continue;
                break;
              }
              if (r == 0)
                break;
              if (send_all(client_fd, filebuf, (size_t)r) != 0)
                break;
            }
          }
          close(dev_fd);
        }
      }

      pthread_mutex_unlock(&g_file_mutex);

      // restore saved char and advance start
      *(nl + 1) = saved;
      start = nl + 1;
    }

    // keep leftover partial data
    size_t remaining = (size_t)((line + line_len) - start);
    memmove(line, start, remaining);
    line_len = remaining;
    line[line_len] = '\0';
  }

  free(line);
  shutdown(client_fd, SHUT_RDWR);
  close(client_fd);

  node->thread_complete = true;
  return NULL;
}

static void reap_completed_threads(void) {
  struct thread_node *cur = SLIST_FIRST(&g_threads);
  struct thread_node *tmp;

  while (cur) {
    tmp = SLIST_NEXT(cur, entries);

    if (cur->thread_complete) {
      pthread_join(cur->thread_id, NULL);
      SLIST_REMOVE(&g_threads, cur, thread_node, entries);
      free(cur);
    }

    cur = tmp;
  }
}

int main(int argc, char *argv[]) {
  // Will prevent other arugments from beinng passed when compiling the file
  // /file -d and /file Only ones allowed
  bool do_daemon = false;
  if (argc == 2 && strcmp(argv[1], "-d") == 0) {
    do_daemon = true;
  } else if (argc != 1) {
    fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
    return 1;
  }

  openlog("aesdsocket", LOG_PID, LOG_USER);

  // Signals
  // Calls handle signal so that the termianl can interup
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
    syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
    closelog();
    return 1;
  }
  // If the daemonize function fails it tells us
  if (do_daemon) {
    if (daemonize() != 0) {
      syslog(LOG_ERR, "daemonize failed: %s", strerror(errno));
      closelog();
      return 1;
    }
  }

  // 1) socket()
  g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_server_fd < 0) {
    syslog(LOG_ERR, "socket failed: %s", strerror(errno));
    closelog();
    return 1;
  }

  // 2) setsockopt(SO_REUSEADDR)
  int opt = 1;
  if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
      0) {
    syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
    close(g_server_fd);
    g_server_fd = -1;
    closelog();
    return 1;
  }

  // 3) bind()
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(PORT);

  if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    syslog(LOG_ERR, "bind failed: %s", strerror(errno));
    close(g_server_fd);
    g_server_fd = -1;
    closelog();
    return 1;
  }

  // 4) listen()
  if (listen(g_server_fd, BACKLOG) < 0) {
    syslog(LOG_ERR, "listen failed: %s", strerror(errno));
    close(g_server_fd);
    g_server_fd = -1;
    closelog();
    return 1;
  }

  while (!g_exit_requested) {
    // accept() // I uncommented this accept function
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EINTR || g_exit_requested)
        break;
      syslog(LOG_ERR, "accept failed: %s", strerror(errno));
      continue;
    }
    // Logs the client IP address
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    syslog(LOG_INFO, "Accepted connection from %s", ip);

    // Allocate thread node
    struct thread_node *node = calloc(1, sizeof(*node));
    if (!node) {
      syslog(LOG_ERR, "calloc failed");
      close(client_fd);
      continue;
    }

    node->client_fd = client_fd;
    node->thread_complete = false;

    // Spawn thread
    if (pthread_create(&node->thread_id, NULL, client_thread_func, node) != 0) {
      syslog(LOG_ERR, "pthread_create(client) failed: %s", strerror(errno));
      close(client_fd);
      free(node);
      continue;
    }

    // Add to singly linked list
    SLIST_INSERT_HEAD(&g_threads, node, entries);

    // Join any completed threads
    reap_completed_threads();
  }

  // Cleanup
  if (g_server_fd != -1) {
    close(g_server_fd);
    g_server_fd = -1;
  }
  g_exit_requested = 1;
  struct thread_node *cur;
  SLIST_FOREACH(cur, &g_threads, entries) {
    if (cur->client_fd != -1) {
      shutdown(cur->client_fd, SHUT_RDWR);
    }
  }
  while (!SLIST_EMPTY(&g_threads)) {
    cur = SLIST_FIRST(&g_threads);
    pthread_join(cur->thread_id, NULL);
    SLIST_REMOVE_HEAD(&g_threads, entries);
    // client_thread_func closes client_fd, but closing again is safe if already
    // closed. If you prefer, you can omit the close here.
    if (cur->client_fd != -1) {
      close(cur->client_fd);
      cur->client_fd = -1;
    }
    free(cur);
  }
  pthread_mutex_destroy(&g_file_mutex);

  syslog(LOG_INFO, "Exiting");
  closelog();
  return 0;
}
