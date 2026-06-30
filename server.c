/*
 * server.c - Multi-client chat server
 * Compile: gcc -o server server.c -lpthread
 * Run:     ./server <port>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define MAX_CLIENTS     50
#define BUFFER_SIZE     2048
#define USERNAME_LEN    32
#define PASSWORD_LEN    64
#define USERS_FILE      "users.txt"
#define HISTORY_FILE    "chat_history.txt"

typedef struct {
    int     sockfd;
    char    username[USERNAME_LEN];
    int     authenticated;
    struct sockaddr_in addr;
} Client;

static Client  *clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── helpers ─────────────────────────────────────────────── */

static void timestamp(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
}

static void save_history(const char *line) {
    FILE *f = fopen(HISTORY_FILE, "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

static void send_history(int sockfd) {
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) return;
    char line[BUFFER_SIZE];
    send(sockfd, "--- Chat History ---\n", 21, 0);
    while (fgets(line, sizeof(line), f))
        send(sockfd, line, strlen(line), 0);
    send(sockfd, "--- End of History ---\n", 23, 0);
    fclose(f);
}

/* ── client list ─────────────────────────────────────────── */

static void add_client(Client *c) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) { clients[i] = c; break; }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void remove_client(int sockfd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->sockfd == sockfd) {
            clients[i] = NULL; break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static Client *find_client(const char *username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && strcmp(clients[i]->username, username) == 0)
            return clients[i];
    }
    return NULL;
}

/* ── broadcast / private ─────────────────────────────────── */

static void broadcast(const char *msg, int exclude_fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->authenticated &&
            clients[i]->sockfd != exclude_fd)
            send(clients[i]->sockfd, msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void send_online_list(int sockfd) {
    char buf[BUFFER_SIZE] = "Online users: ";
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->authenticated) {
            strcat(buf, clients[i]->username);
            strcat(buf, " ");
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    strcat(buf, "\n");
    send(sockfd, buf, strlen(buf), 0);
}

/* ── authentication ──────────────────────────────────────── */

/* Returns 1 if username exists with matching password, 0 otherwise.
   Returns -1 if username exists but password is wrong. */
static int check_credentials(const char *user, const char *pass) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return 0;
    char u[USERNAME_LEN], p[PASSWORD_LEN];
    while (fscanf(f, "%31s %63s", u, p) == 2) {
        if (strcmp(u, user) == 0) {
            fclose(f);
            return strcmp(p, pass) == 0 ? 1 : -1;
        }
    }
    fclose(f);
    return 0; /* not found */
}

static void register_user(const char *user, const char *pass) {
    FILE *f = fopen(USERS_FILE, "a");
    if (f) { fprintf(f, "%s %s\n", user, pass); fclose(f); }
}

/* Returns 1 on success, 0 on failure */
static int authenticate(Client *c) {
    char buf[BUFFER_SIZE];
    int  n;

    send(c->sockfd, "CMD:AUTH\n", 9, 0); /* tell client to start auth */

    /* receive: LOGIN <user> <pass>  or  REGISTER <user> <pass> */
    n = recv(c->sockfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    char cmd[16], user[USERNAME_LEN], pass[PASSWORD_LEN];
    if (sscanf(buf, "%15s %31s %63s", cmd, user, pass) != 3) {
        send(c->sockfd, "ERR:Bad format\n", 15, 0);
        return 0;
    }

    if (strcmp(cmd, "REGISTER") == 0) {
        if (check_credentials(user, pass) != 0) {
            send(c->sockfd, "ERR:Username taken\n", 19, 0);
            return 0;
        }
        register_user(user, pass);
        strncpy(c->username, user, USERNAME_LEN - 1);
        send(c->sockfd, "OK:Registered\n", 14, 0);
        return 1;
    }

    if (strcmp(cmd, "LOGIN") == 0) {
        int r = check_credentials(user, pass);
        if (r == 1) {
            /* check duplicate session */
            pthread_mutex_lock(&clients_mutex);
            Client *dup = find_client(user);
            pthread_mutex_unlock(&clients_mutex);
            if (dup) {
                send(c->sockfd, "ERR:Already logged in\n", 22, 0);
                return 0;
            }
            strncpy(c->username, user, USERNAME_LEN - 1);
            send(c->sockfd, "OK:Login\n", 9, 0);
            return 1;
        } else if (r == -1) {
            send(c->sockfd, "ERR:Wrong password\n", 19, 0);
        } else {
            send(c->sockfd, "ERR:User not found\n", 19, 0);
        }
        return 0;
    }

    send(c->sockfd, "ERR:Unknown command\n", 20, 0);
    return 0;
}

/* ── command handling ────────────────────────────────────── */

static void handle_command(Client *c, const char *input) {
    char ts[32];
    timestamp(ts, sizeof(ts));

    if (strcmp(input, "/help") == 0) {
        const char *help =
            "Commands:\n"
            "  /help              - Show this help\n"
            "  /online            - List online users\n"
            "  /msg <user> <text> - Private message\n"
            "  /exit              - Disconnect\n";
        send(c->sockfd, help, strlen(help), 0);

    } else if (strcmp(input, "/online") == 0) {
        send_online_list(c->sockfd);

    } else if (strncmp(input, "/msg ", 5) == 0) {
        char target[USERNAME_LEN], msg[BUFFER_SIZE];
        if (sscanf(input + 5, "%31s %[^\n]", target, msg) < 2) {
            send(c->sockfd, "ERR:Usage: /msg <user> <message>\n", 33, 0);
            return;
        }
        pthread_mutex_lock(&clients_mutex);
        Client *dest = find_client(target);
        pthread_mutex_unlock(&clients_mutex);
        if (!dest) {
            send(c->sockfd, "ERR:User not online\n", 20, 0);
            return;
        }
        char pm[BUFFER_SIZE * 2];
        snprintf(pm, sizeof(pm), "[PM from %s]: %s\n", c->username, msg);
        send(dest->sockfd, pm, strlen(pm), 0);
        snprintf(pm, sizeof(pm), "[PM to %s]: %s\n", target, msg);
        send(c->sockfd, pm, strlen(pm), 0);

    } else if (strcmp(input, "/exit") == 0) {
        /* handled by caller */

    } else {
        send(c->sockfd, "ERR:Unknown command. Type /help\n", 32, 0);
    }
}

/* ── per-client thread ───────────────────────────────────── */

static void *client_thread(void *arg) {
    Client *c = (Client *)arg;
    char    buf[BUFFER_SIZE];
    char    msg[BUFFER_SIZE * 2];
    char    ts[32];
    int     n;

    /* authenticate (up to 3 attempts) */
    int authed = 0;
    for (int attempt = 0; attempt < 3 && !authed; attempt++)
        authed = authenticate(c);

    if (!authed) {
        send(c->sockfd, "ERR:Authentication failed\n", 26, 0);
        close(c->sockfd);
        free(c);
        return NULL;
    }

    c->authenticated = 1;
    add_client(c);

    /* send history then announce */
    send_history(c->sockfd);

    timestamp(ts, sizeof(ts));
    snprintf(buf, sizeof(buf), "[%s] *** %s joined the chat ***\n", ts, c->username);
    printf("%s", buf);
    broadcast(buf, c->sockfd);
    save_history(buf);

    /* main receive loop */
    while ((n = recv(c->sockfd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) continue;

        if (buf[0] == '/') {
            if (strcmp(buf, "/exit") == 0) break;
            handle_command(c, buf);
        } else {
            timestamp(ts, sizeof(ts));
            char msg[BUFFER_SIZE];
            snprintf(msg, sizeof(msg), "[%s] %.31s: %.1900s\n", ts, c->username, buf);
            printf("%s", msg);
            broadcast(msg, c->sockfd);
            /* echo back to sender */
            send(c->sockfd, msg, strlen(msg), 0);
            save_history(msg);
        }
    }

    /* cleanup */
    timestamp(ts, sizeof(ts));
    snprintf(buf, sizeof(buf), "[%s] *** %s left the chat ***\n", ts, c->username);
    printf("%s", buf);
    broadcast(buf, c->sockfd);
    save_history(buf);

    remove_client(c->sockfd);
    close(c->sockfd);
    free(c);
    return NULL;
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }

    printf("Server listening on port %d\n", port);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cli_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) { perror("accept"); continue; }

        Client *c = calloc(1, sizeof(Client));
        c->sockfd = cli_fd;
        c->addr   = cli_addr;

        printf("New connection from %s:%d\n",
               inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, c);
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
