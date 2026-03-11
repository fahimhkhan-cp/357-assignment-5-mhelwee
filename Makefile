CC = gcc
CFLAGS = -Wall -std=c99 -pedantic
HTTPD = httpd
HTTPD_OBJS = httpd.o net.o
all : $(HTTPD)
$(HTTPD) : $(HTTPD_OBJS)
	$(CC) $(CFLAGS) -o $(HTTPD) $(HTTPD_OBJS)
httpd.o : httpd.c net.h
	$(CC) $(CFLAGS) -c httpd.c
net.o : net.c net.h
	$(CC) $(CFLAGS) -c net.c
clean :
	rm -f *.o $(HTTPD) core
