#define _GNU_SOURCE
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

void handle_request(int nfd)
{
    char buf[4096];
    ssize_t num = read(nfd, buf, sizeof(buf)-1);

    if (num <= 0)
    {
        close(nfd);
        return;
    }

    buf[num] = '\0';

    char method[16], path[256], version[16];
    int parsed = sscanf(buf, "%s %s %s", method, path, version);

    if (parsed != 3)
    {
        char err[] = "HTTP/1.0 400 Bad Request\r\n\r\n";
        write(nfd, err, strlen(err));
        close(nfd);
        return;
    }

    printf("Request: %s %s\n", method, path);
    fflush(stdout);

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
    {
        char err[] = "HTTP/1.0 501 Not Implemented\r\n\r\n";
        write(nfd, err, strlen(err));
        close(nfd);
        return;
    }

    char *filepath = path;
    if (filepath[0] == '/')
        filepath++;

    if (strstr(filepath, "..") != NULL)
    {
        char err[] = "HTTP/1.0 403 Permission Denied\r\n\r\n";
        write(nfd, err, strlen(err));
        close(nfd);
        return;
    }

    /* cgi-like handling */
    if (strncmp(filepath, "cgi-like/", 9) == 0)
    {
        char cgibuf[256];
        strncpy(cgibuf, filepath + 9, sizeof(cgibuf)-1);
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
            char err[] = "HTTP/1.0 500 Internal Error\r\n\r\n";
            write(nfd, err, strlen(err));
            close(nfd);
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
            char err[] = "HTTP/1.0 500 Internal Error\r\n\r\n";
            write(nfd, err, strlen(err));
            close(nfd);
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
            char fbuf[4096];
            size_t bytes;
            while ((bytes = fread(fbuf, 1, sizeof(fbuf), f)) > 0)
                write(nfd, fbuf, bytes);
            fclose(f);
        }
        remove(tmpfile);
        close(nfd);
        return;
    }

    /* regular file handling */
    struct stat st;
    if (stat(filepath, &st) == -1)
    {
        char err[] = "HTTP/1.0 404 Not Found\r\n\r\n";
        write(nfd, err, strlen(err));
        close(nfd);
        return;
    }

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\n\r\n",
        st.st_size);
    write(nfd, header, strlen(header));

    if (strcmp(method, "GET") == 0)
    {
        FILE *f = fopen(filepath, "r");
        if (f == NULL)
        {
            close(nfd);
            return;
        }
        char fbuf[4096];
        size_t bytes;
        while ((bytes = fread(fbuf, 1, sizeof(fbuf), f)) > 0)
            write(nfd, fbuf, bytes);
        fclose(f);
    }

    close(nfd);
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
                                                                                    
