/*
 * client.c - Chat client
 * Compile: gcc -o client client.c -lpthread
 * Run:     ./client <server_ip> <port>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUFFER_SIZE  2048
#define USERNAME_LEN 32
#define PASSWORD_LEN 64

static int sockfd;
static volatile int running = 1;

/* ── receive thread ──────────────────────────────────────── */

static void *recv_thread(void *arg) {
    (void)arg;
    char buf[BUFFER_SIZE];
    int  n;
    while (running && (n = recv(sockfd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
    running = 0;
    printf("\nDisconnected from server.\n");
    return NULL;
}

/* ── authentication ──────────────────────────────────────── */

static int do_auth(void) {
    char buf[BUFFER_SIZE];
    int  n;

    /* wait for CMD:AUTH */
    n = recv(sockfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    buf[n] = '\0';
    if (strncmp(buf, "CMD:AUTH", 8) != 0) {
        printf("%s", buf);
        return 0;
    }

    printf("=== Chat Login ===\n");
    printf("1) Login\n2) Register\nChoice: ");
    fflush(stdout);

    char choice[4];
    if (!fgets(choice, sizeof(choice), stdin)) return 0;

    char user[USERNAME_LEN], pass[PASSWORD_LEN], msg[BUFFER_SIZE];

    printf("Username: "); fflush(stdout);
    if (!fgets(user, sizeof(user), stdin)) return 0;
    user[strcspn(user, "\r\n")] = '\0';

    printf("Password: "); fflush(stdout);
    if (!fgets(pass, sizeof(pass), stdin)) return 0;
    pass[strcspn(pass, "\r\n")] = '\0';

    const char *cmd = (choice[0] == '2') ? "REGISTER" : "LOGIN";
    snprintf(msg, sizeof(msg), "%s %s %s\n", cmd, user, pass);
    send(sockfd, msg, strlen(msg), 0);

    /* read server response */
    n = recv(sockfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    buf[n] = '\0';
    printf("%s", buf);
    fflush(stdout);

    return strncmp(buf, "OK:", 3) == 0;
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(atoi(argv[2]))
    };
    if (inet_pton(AF_INET, argv[1], &srv.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[1]);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect"); return 1;
    }
    printf("Connected to %s:%s\n", argv[1], argv[2]);

    if (!do_auth()) {
        printf("Authentication failed. Exiting.\n");
        close(sockfd);
        return 1;
    }

    printf("Type /help for commands.\n\n");

    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);
    pthread_detach(tid);

    char buf[BUFFER_SIZE];
    while (running && fgets(buf, sizeof(buf), stdin)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) continue;

        /* append newline for server parsing */
        char msg[BUFFER_SIZE + 2];
        snprintf(msg, sizeof(msg), "%s\n", buf);
        send(sockfd, msg, strlen(msg), 0);

        if (strcmp(buf, "/exit") == 0) break;
    }

    running = 0;
    close(sockfd);
    return 0;
}
