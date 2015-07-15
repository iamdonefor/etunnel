#include <stdio.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <sys/poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <sys/select.h>

typedef unsigned int u32;
typedef unsigned short u16;

struct gconfig {
    u32 from_ip;
    u32 to_ip;
    u16 from_port;
    u16 to_port;
    u32 bind_ip;
    int nlisten;
};

struct connection {
    int local;
    int remote;
    struct connection * next;
};

#define CHUNK_SIZE 65536

/* global variables */
struct connection *conn_head = NULL;
pthread_mutex_t conn_lock=PTHREAD_MUTEX_INITIALIZER;
int debug=0;

/* ------------------------------------------------------------------ */

void usage(char * selfname) {
    printf("Usage: %s -f address:port -t address:port [-b bind_to_forward]\n", selfname);
    exit(-1);
}

void fail(char * message) {
    perror(message);
    exit(-1);
}

int arg_to_addr_port(char *a, u32 *ip, u16 *port) {
    char *semi = strchr(a ? a : "", ':');

    if (!semi) return -1;
    *semi++ = 0;                // sets ':' to zero THEN increases semi pointer

    *ip = inet_addr(a);
    *port = atoi(semi);

    return 0;
}

void clean_socket(int s) {
    shutdown(s,2);
    close(s);
}

/* ------------------------------------------------------------------ */
void alloc_connection(int local, int remote) {
    struct connection *new;

    new = calloc(1, sizeof(struct connection));
    new->local = local;
    new->remote = remote;

    pthread_mutex_lock(&conn_lock);
    new->next = conn_head;
    conn_head = new;
    pthread_mutex_unlock(&conn_lock);


    if (debug) printf("NEW CONNECTION (%p): %d -> %d\n", new, local, remote);
}

/* XXX: must be called inside lock */
void kill_connection(struct connection *killc) {
    struct connection *c = conn_head, *cprev = conn_head;

    if (debug) printf("REMOVING (%p) %d -> %d head:%p\n", killc, killc->local, killc->remote, conn_head);

    if (killc == conn_head) {
        conn_head = conn_head->next;

        clean_socket(killc->local);
        clean_socket(killc->remote);

        free(killc);
        return;
    }

    while(c) {
        if (killc == c) {
            cprev->next = c->next;
            clean_socket(killc->local);
            clean_socket(killc->remote);
            free(killc);
            return;
        }

        cprev = c;
        c = c->next;
    }

}

/* ------------------------------------------------------------------ */
void * do_accept(void * arg) {
    struct gconfig gc = *((struct gconfig *)arg);
    struct sockaddr_in sa, dummy;
    int s, new, new_remote, dummy_len, on=1;

    memset(&sa, 0, sizeof(sa));

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s==-1) fail("socket");

    sa.sin_family   = AF_INET;
    sa.sin_port     = htons(gc.from_port);

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,(char *)&on, sizeof(on));

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == -1) fail("bind");
    listen(s, gc.nlisten);

    while(1) {
        new = accept(s, (struct sockaddr *)&dummy, &dummy_len);
        if (new == -1) {
            perror("accept:");
            continue;
        }

        new_remote = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        // bind here to local address if needed
        if (gc.bind_ip) {
            memset(&dummy, 0, sizeof(dummy));
            dummy.sin_family = AF_INET;
            dummy.sin_addr.s_addr = gc.bind_ip;
            if (bind(new_remote, (struct sockaddr *)&dummy, sizeof(dummy)) == -1) {
                perror("bind2:");
                clean_socket(new);
                clean_socket(new_remote);
                continue;
            }
        }

        memset(&dummy, 0, sizeof(dummy));
        dummy.sin_family = AF_INET;
        dummy.sin_port  = htons(gc.to_port);
        dummy.sin_addr.s_addr = gc.to_ip;
        if (connect(new_remote, (struct sockaddr *)&dummy, sizeof (dummy))) {
            perror("connect:");
            clean_socket(new);
            clean_socket(new_remote);
            continue;
        }

        
        alloc_connection(new, new_remote);
    }

}

int here_there(int here, int there) {
    char chunk[CHUNK_SIZE];
    int r;

    r = read(here, chunk, sizeof(chunk));
    if (debug) {
        int i, l=r;

        printf("%d -> %d\n", here, there);
    
        if(l>1024) l=1024;

        for(i=0; i<l; i++) printf("%c", chunk[i]);
        printf("\n");
    }
    if (r<=0) { if (debug) printf (">> DEAD READ << (%d)\n", r); return 1; }

    r = write(there, chunk, r);
    if (r<=0) { if (debug) printf (">> DEAD WRITE << (%d)\n", r); return 1; }

    if (debug) printf(">> OK <<\n");

    return 0;
}


void * do_redirect(void * arg) {
    struct gconfig gc = *((struct gconfig *)arg);
    fd_set conn_fds;
    int n, dead;
    struct connection *c, *killc;
    struct timeval to;

    while (1) {
        if (debug) fflush(stdout);

        pthread_mutex_lock(&conn_lock);

        c = conn_head;
        n = 0;
        FD_ZERO(&conn_fds);

        while (c) {
            FD_SET(c->local, &conn_fds);
            FD_SET(c->remote, &conn_fds);

            if (c->local > n) n = c->local;
            if (c->remote > n) n = c->remote;

            c = c->next;
        }

        pthread_mutex_unlock(&conn_lock);

        to.tv_sec = 0;
        to.tv_usec = 1000;
        n = select(n+1, &conn_fds, NULL, NULL, &to);

        if (!n) continue;

        pthread_mutex_lock(&conn_lock);

        c = conn_head;
        while (c) {
            dead = 0;

            if (FD_ISSET(c->local, &conn_fds)) {
                dead = here_there(c->local, c->remote);
            }

            if (FD_ISSET(c->remote, &conn_fds)) {
                dead += here_there(c->remote, c->local);
            }

            if (dead) {
                killc = c;
                c = c->next;
                kill_connection(killc);
                continue;
            }
                                            
            c = c->next;
            continue;
        }

        pthread_mutex_unlock(&conn_lock);
    }
}


int main(int argc, char ** argv) {
    int i;
    char *bind_address=NULL, *from=NULL, *to=NULL;
    struct gconfig gc;
    pthread_t accept_thread, redirect_thread;

    gc.bind_ip = 0;
    gc.nlisten = 32;

    /* parse parameters */
    while ((i=getopt(argc, argv, "f:t:b:d")) != -1) {
        switch (i) {
            case 'b':
                bind_address = optarg;
                break;
            case 'f':
                from = optarg;
                break;
            case 't':
                to = optarg;
                break;
            case 'd':
                debug = 1;
                break;
            default:
                usage(argv[0]);
                break;
        }
    }

    if (arg_to_addr_port(from, &gc.from_ip, &gc.from_port)) usage(argv[0]);
    if (arg_to_addr_port(to, &gc.to_ip, &gc.to_port)) usage(argv[0]);
    if (bind_address) gc.bind_ip = inet_addr(bind_address);

    // spawn main thread and redirecting thread

    pthread_create(&redirect_thread, NULL, do_redirect, (void*)&gc);
    do_accept((void *)&gc);
}
