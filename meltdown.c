/*
 * meltdown - sparkfun freeday ticket request proxy
 * 
 * Ben LeMasurier <ben@sparkfun.com> 2k11'
 *
 * gcc -lpthread meltdown.c -o meltdown
 *
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#define MAXEVENTS 64

struct meltdown {
  int ticket_port;
  int yay_bit;
} meltdown;

static unsigned int
win(void)
{
  if(meltdown.yay_bit) {
    meltdown.yay_bit = 0;
    return 1;
  }

  return 0;
}

static int
make_socket_non_blocking(int sfd)
{
  int flags, s;

  if((flags = fcntl(sfd, F_GETFL, 0)) == -1) {
    perror("fcntl");
    return -1;
  }

  flags |= O_NONBLOCK;
  if((s = fcntl(sfd, F_SETFL, flags)) == -1) {
    perror("fcntl");
    return -1;
  }

  return 0;
}

static int
create_and_bind(char *port)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s, sfd;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family   = AF_INET;     /* IPv4 only. */
  hints.ai_socktype = SOCK_STREAM; /* TCP socket */
  hints.ai_flags    = AI_PASSIVE;  /* All interfaces */

  if((s = getaddrinfo(NULL, port, &hints, &result)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
    return -1;
  }

  for(rp = result; rp != NULL; rp = rp->ai_next) {
    sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if(sfd == -1)
      continue;

    /* successful bind */
    if((s = bind(sfd, rp->ai_addr, rp->ai_addrlen)) == 0)
      break;

    close(sfd);
  }

  if(rp == NULL) {
    fprintf(stderr, "could not bind\n");
    return -1;
  }

  freeaddrinfo(result);

  return sfd;
}

void
get_ticket_listener(char *port)
{
  int sfd, s;
  int efd;
  struct epoll_event event;
  struct epoll_event *events;
  char *win_yes = "1";
  char *win_no  = "0";
  size_t yes_len = strlen(win_yes);
  size_t no_len  = strlen(win_no);

  if((sfd = create_and_bind(port)) == -1)
    abort();

  if((s = make_socket_non_blocking(sfd)) == -1)
    abort();

  if((s = listen(sfd, SOMAXCONN)) == -1) {
    perror("listen");
    abort();
  }

  if((efd = epoll_create1(0)) == -1) {
    perror("epoll_create");
    abort();
  }

  event.data.fd = sfd;
  event.events = EPOLLIN | EPOLLET;
  if((s = epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &event)) == -1) {
    perror("epoll_ctl");
    abort();
  }

  /* buffer where events are returned */
  events = calloc(MAXEVENTS, sizeof(event));

  /* event loop */
  while(1) {
    int n, i;

    n = epoll_wait(efd, events, MAXEVENTS, -1);
    for(i = 0; i < n; i++) {
      if((events[i].events & EPOLLERR) ||
         (events[i].events & EPOLLHUP) ||
         (!(events[i].events & EPOLLIN)))
      {
        /* error on this fd, or the socket is not ready (wtf.) */
        fprintf(stderr, "epoll error\n");
        close(events[i].data.fd);
        continue;
      } else if(sfd == events[i].data.fd) {
        /* notification on the listening socket */
        while(1) {
          struct sockaddr in_addr;
          socklen_t in_len;
          int infd;
          char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

          in_len = sizeof in_addr;
          infd = accept(sfd, &in_addr, &in_len);
          if(infd == -1) {
            if((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
              /* all incoming connections processed. */
              break;
            } else {
              perror("accept");
              break;
            }
          }

          s = getnameinfo(&in_addr, in_len,
              hbuf, sizeof hbuf,
              sbuf, sizeof sbuf,
              NI_NUMERICHOST | NI_NUMERICSERV);

          if(s == 0)
            printf("Accepted connection on descriptor %d "
                "(host=%s, port=%s)\n", infd, hbuf, sbuf);

          if((s = make_socket_non_blocking(infd)) == -1)
            abort();

          event.data.fd = infd;
          event.events = EPOLLIN | EPOLLET;
          s = epoll_ctl(efd, EPOLL_CTL_ADD, infd, &event);
          if(s == -1) {
            perror("epoll_ctl");
            abort();
          }

          /* inform client of success/failure */
          if(win())
            send(infd, win_yes, yes_len, MSG_DONTWAIT);
          else
            send(infd, win_no, no_len, MSG_DONTWAIT);

          close(infd);
        }
        continue;
      }
    }
  }

  free(events);
  close(sfd);
}

void *
set_ticket_listener()
{
  int server_socket, client_socket;
  struct sockaddr_in server, client;

  if((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    abort();
  }

  memset(&server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = htonl(INADDR_ANY);
  server.sin_port = htons(meltdown.ticket_port);

  if(bind(server_socket, (struct sockaddr *) &server, sizeof(server)) == -1) {
    perror("bind");
    abort();
  }

  if(listen(server_socket, SOMAXCONN) < 0) {
    perror("listen");
    abort();
  }

  while(1) {
    /* wait for tickets */
    size_t client_len = sizeof(client);
    if((client_socket = accept(server_socket, (struct sockaddr *) &client, (socklen_t *) &client_len)) < 0)
      perror("accept");

    meltdown.yay_bit = 1;

    close(client_socket);
  }
}

int
main(int argc, char *argv[])
{
  pthread_t ticket_thread;
  meltdown.yay_bit = 0;

  if(argc != 3) {
    fprintf(stderr, "Usage: %s [set ticket port] [get ticket port]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  meltdown.ticket_port = atoi(argv[1]);

  pthread_create(&ticket_thread, NULL, set_ticket_listener, NULL);
  get_ticket_listener(argv[2]);

  pthread_exit(NULL);

  return EXIT_SUCCESS;
}
