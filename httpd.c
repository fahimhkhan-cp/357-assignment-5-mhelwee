#define _GNU_SOURCE
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

void send_error(int nfd, const char *status, const char *message)
{
    char body[256];
    snprintf(body, sizeof(body), "<html><body><h1>%s</h1></body></html>", message);
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.0 %s\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n\r\n",
        status, strlen(body));
    write(nfd, header, strlen(header));
    write(nfd, body, strlen(body));
}

void send_file(int nfd, const char *filepath, struct stat *st, int send_body)
{
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\n\r\n",
        st->st_size);
    write(nfd, header, strlen(header));

    if (!send_body)
        return;

    FILE *f = fopen(filepath, "r");
    if (f == NULL)
        return;

    char buf[4096];
    size_t bytes;
    while ((bytes = fread(buf, 1, sizeof(buf), f)) > 0)
        write(nfd, buf, bytes);

    fclose(f);
}

void handle_cgi(int nfd, const char *filepath_full, const char *cgibuf_in)
{
    char cgibuf[256];
    strncpy(cgibuf, cgibuf_in, sizeof(cgibuf)-1);
    cgibuf[sizeof(cgibuf)-1] = '\0';

    char *args[64];
    int argc = 0;
    char progpath[256];

    char *question = strchr(cgibuf, '?');
    if (question != NULL)
    {
        *question = '\0';
        question++;
        snprintf(progpath, sizeof(progpath), "cgi-like/%s", cgibuf);
        args[argc++] = cgibuf;
        char *token = strtok(question, "&");
        while (token != NULL && argc < 63)
        {
            args[argc++] = token;
            token = strtok(NULL, "&");
        }
    }
    else
    {
        snprintf(progpath, sizeof(progpath), "cgi-like/%s", cgibuf);
        args[argc++] = cgibuf;
    }
    args[argc] = NULL;

    char tmpfile[64];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/cgi_%d.tmp", getpid());

    pid_t pid = fork();
    if (pid == -1)
    {
        send_error(nfd, "500 Internal Error", "500 Internal Error");
        return;
    }
    if (pid == 0)
    {
        int fd = open(tmpfile, O_WRONLY|O_CREAT|O_TRUNC, 0600);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        execv(progpath, args);
        exit(1);
    }
    waitpid(pid, NULL, 0);

    struct stat st;
    if (stat(tmpfile, &st) == -1)
    {
        send_error(nfd, "500 Internal Error", "500 Internal Error");
        return;
    }

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\n\r\n",
        st.st_size);
    write(nfd, header, strlen(header));

    FILE *f = fopen(tmpfile, "r");
    if (f != NULL)
    {
        char buf[4096];
        size_t bytes;
        while ((bytes = fread(buf, 1, sizeof(buf), f)) > 0)
            write(nfd, buf, bytes);
        fclose(f);
    }
    remove(tmpfile);
}

void handle_request(int nfd)
{
    FILE *network = fdopen(nfd, "r");
    if (network == NULL)
    {
        close(nfd);
        return;
    }

    char *line = NULL;
    size_t size = 0;
    ssize_t num = getline(&line, &size, network);

    if (num <= 0)
    {
        free(line);
        fclose(network);
        return;
    }

    char method[16], path[256], version[16];
    int parsed = sscanf(line, "%s %s %s", method, path, version);
    free(line);

    if (parsed != 3)
    {
        send_error(nfd, "400 Bad Request", "400 Bad Request");
        fclose(network);
        return;
    }

    printf("Request: %s %s\n", method, path);
    fflush(stdout);

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
    {
        send_error(nfd, "501 Not Implemented", "501 Not Implemented");
        fclose(network);
        return;
    }

    char *filepath = path;
    if (filepath[0] == '/')
        filepath++;

    if (strstr(filepath, "..") != NULL)
    {
        send_error(nfd, "403 Permission Denied", "403 Permission Denied");
        fclose(network);
        return;
    }

    if (strncmp(filepath, "cgi-like/", 9) == 0)
    {
        handle_cgi(nfd, filepath, filepath + 9);
        fclose(network);
        return;
    }

    struct stat st;
    if (stat(filepath, &st) == -1)
    {
        send_error(nfd, "404 Not Found", "404 Not Found");
        fclose(network);
        return;
    }

    send_file(nfd, filepath, &st, strcmp(method, "GET") == 0);
    fclose(network);
}

void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void run_service(int fd)
{
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    while (1)
    {
        int nfd = accept_connection(fd);
        if (nfd != -1)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                close(fd);
                handle_request(nfd);
                exit(0);
            }
            else if (pid > 0)
            {
                close(nfd);
            }
            else
            {
                perror("fork");
                close(nfd);
            }
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    if (port <= 0)
    {
        fprintf(stderr, "Invalid port\n");
        exit(1);
    }

    int fd = create_service(port);
    if (fd == -1)
    {
        perror(0);
        exit(1);
    }

    printf("listening on port: %d\n", port);
    fflush(stdout);
    run_service(fd);
    close(fd);
    return 0;
}
