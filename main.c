#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/stat.h>
#include <string.h>

#include "http_parser.h"

#define WORKERS_AMOUNT 4
#define WORKER_BUF_SIZE 16
#define MAX_EVENTS 32
#define LOG_ENABLE 1
#define LENGTH_STRING_NOT_FOUND 53

const char* log_name = "./log.txt";

int start_worker_ptr = 0;

const char* templ = "HTTP/1.0 200 OK\r\n"
                    "Content-length: %d\r\n"
                    "Connection: close\r\n"
                    "Content-Type: text/html\r\n"
                    "\r\n"
                    "%s";

const char* not_found = "HTTP/1.0 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n";

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

char logger_buf[1024];

struct globalArgs_t {
    char *char_host;
	char *char_port;
	char *dir;
	int i32_host;
	int i32_port;
} globalArgs;

typedef struct {
	char buff[1024];
} url_data;

ssize_t
sock_fd_write(int sock, void *buf, ssize_t buflen, int fd);

ssize_t
sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd);

void skeleton_daemon();

int set_nonblock(int fd);

int on_url_cb(http_parser* parser, const char* at, size_t length) {
	strncpy(((url_data*)parser->data)->buff, at, length);
	return 0;
}

void logger(const char *msg) {
	if (LOG_ENABLE) {
		pthread_mutex_lock(&mutex);
		FILE* fd = fopen(log_name, "a");
		fprintf(fd, "%s", msg); 
		fclose(fd);
		pthread_mutex_unlock(&mutex);
	}
}

int get_server_addr(char* addr_str) {
	int i32_addr = 0; 
	i32_addr = inet_addr(addr_str);
	if (i32_addr == 0) {
		logger("Fail convert IP addres\n");
	}
	return i32_addr;
}

int rr_work_disp(int *sv, int sv_len, int *fd, int fd_len) {
	// round robin socket send to workers
	int current_worker = start_worker_ptr;
	char fbuf[1] = {0};
	int fbuf_len = 1;
	size_t size = {0};
	pid_t pid = getpid();

	for (int i = 0; i < fd_len; ++i) {
		size = sock_fd_write(sv[current_worker*2 + 1], fbuf, fbuf_len, fd[i]);
        sprintf(logger_buf, "pass fd#%i to w#%i, %i, %i\n", fd[i], current_worker, i, pid);
		logger(logger_buf);
		if (size < 0) {
			return -1;
		}
		current_worker = current_worker >= WORKERS_AMOUNT - 1 ? 0 : current_worker + 1;
	}

	start_worker_ptr = current_worker;
	return 0;
}

int
dispacher(int *sv, int sv_len)
{
	int MasterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in SockAddr;
	SockAddr.sin_family 		= AF_INET;
	SockAddr.sin_port 			= globalArgs.i32_port;
	SockAddr.sin_addr.s_addr 	= globalArgs.i32_host;
	
	bind(MasterSocket, (struct sockaddr *)(&SockAddr), sizeof(SockAddr));
	set_nonblock(MasterSocket);
	listen(MasterSocket, SOMAXCONN);

	int EPoll = epoll_create1(0);

	// register mastersocket
	struct epoll_event Event;
	Event.data.fd = MasterSocket;
	Event.events = EPOLLIN;
	epoll_ctl(EPoll, EPOLL_CTL_ADD, MasterSocket, &Event);

	while(1) {
		struct epoll_event Events[MAX_EVENTS];
		int N = epoll_wait(EPoll, Events, MAX_EVENTS, -1);
        
		// run through all events (that guarantee work)
		for (int i = 0; i < N; i++) {
			if (Events[i].data.fd == MasterSocket) {
				int SlaveSocket = accept(MasterSocket, 0, 0);
				if (rr_work_disp(sv, sv_len, &SlaveSocket, 1)) {
					logger("Error while dispatching file descriptors\n");
				}
				sprintf(logger_buf, "i = %i, N = %i\n", i, N);
				logger(logger_buf);
                close(SlaveSocket);
			}
		}
	}

	return 0;
}

void worker(int sock) {
	char internal_buf[1024] = {0};

    int 	fd 		= 0;
    char 	buf[1] 	= {0};
    ssize_t size 	= 0;
	int 	isize	= 0;
    pid_t 	pid 	= getpid();

    int EPoll = epoll_create1(0);

    while (1) {
    	struct epoll_event Events[MAX_EVENTS];
    	size = sock_fd_read(sock, buf, sizeof(buf), &fd);
		if ((size < 0) || (fd == -1)) {
			sleep(0.2);
		}
		else {
			// put socket from dispacter to epoll entity
			set_nonblock(fd);
			struct epoll_event Event;
			Event.data.fd = fd;
			Event.events = EPOLLIN | EPOLLONESHOT;
			epoll_ctl(EPoll, EPOLL_CTL_ADD, fd, &Event);
		}

		int N = epoll_wait(EPoll, Events, MAX_EVENTS, -1);

		// run through all events (that guarantee work)
		for (int i = 0; i < N; i++) {
			char Buffer[1024];
			char output_buffer[1024];
			http_parser* parser = (http_parser*)malloc(sizeof(http_parser));
			http_parser_init(parser, HTTP_REQUEST);
			url_data data;
			bzero(&data, sizeof(data));
			parser->data = &data;

			http_parser_settings settings;
			bzero(&settings, sizeof(settings));
			settings.on_url = on_url_cb;

			int RecvResult = recv(Events[i].data.fd, Buffer, 1024, MSG_NOSIGNAL);

			if ((RecvResult == 0)) {//&& (errno != EAGAIN)) {
				shutdown(Events[i].data.fd, SHUT_RDWR);
				close(Events[i].data.fd);
			} else if (RecvResult > 0) {
				int ParsResult = http_parser_execute(parser, &settings, Buffer, 1024);
				if (ParsResult >= RecvResult) {
					// cut exceess parameters in request
                    for (int i = 0; i < ParsResult; ++i) {
                        if (data.buff[i] == '?') {
                            data.buff[i] = '\0';
                            break;
                        }
                    }
					int file_fd = open((char*)(data.buff + 1), O_RDONLY);
					if (file_fd == -1) {
						size = send(Events[i].data.fd, not_found, 53, MSG_NOSIGNAL);;
					}
					else {
						size = read(file_fd, internal_buf, 1024);
						if (size > 0) {
							isize = sprintf(output_buffer, templ, size, internal_buf);
							if (isize > 0) {
								size = send(Events[i].data.fd, output_buffer, isize, MSG_CONFIRM);
								logger(output_buffer);
                                if (size != isize) {
                                    logger("ERROR SENDING MSG\n");
                                }
							}
							else {
								logger("ERROR while writing final buffer\n");
							}
						}
                        close(file_fd);
					}
				}
				else {
                    sprintf(logger_buf, "fail to Parse! - %i>=%i\n", ParsResult, RecvResult);
					logger("ERROR parsing http request\n");
				}
			}
  
            shutdown(Events[i].data.fd, SHUT_RDWR);
			close(Events[i].data.fd);
			free(parser);
		}
	}
}

int
main(int argc, char **argv)
{	
	globalArgs.char_host = NULL;
	globalArgs.char_port = NULL;
	globalArgs.dir = NULL;
	globalArgs.i32_host = 0;
	globalArgs.i32_port = 0;
	
	int opt = 0;
	char *optString = "h:p:d:";
	opt = getopt( argc, argv, optString );
    while( opt != -1 ) {
        switch( opt ) {
            case 'h':
                globalArgs.char_host = optarg;
                break;
                 
            case 'p':
                globalArgs.char_port = optarg;
                break;
                 
            case 'd':
                globalArgs.dir = optarg;
                break;
                 
            default:
                // impossible to be here 
                break;
        }
        opt = getopt( argc, argv, optString );
    }
	
	globalArgs.i32_host = get_server_addr(globalArgs.char_host);
	globalArgs.i32_port = atoi(globalArgs.char_port);
	globalArgs.i32_port = htons(globalArgs.i32_port);
    
    skeleton_daemon();
		
	pid_t pid = 0;
    int status = 0;

    int sv[WORKERS_AMOUNT * 2] = { 0 };
    int i = 0;
    while (i < WORKERS_AMOUNT * 2) {
        status = socketpair(AF_LOCAL, SOCK_STREAM, 0, sv + i);
		if (status) {
			logger("Fail create Socket Pairs\n");
			exit(EXIT_FAILURE);
		}
        i += 2;
    }

	pid = fork();
	i = 0;
	if (pid != 0) {
		pid = getpid();
		sprintf(logger_buf, "\ndispacher pid=%i\n", pid);
        logger(logger_buf);
		dispacher(sv, WORKERS_AMOUNT * 2);

	}
	while (i < WORKERS_AMOUNT) {
		pid = fork();
		if (pid == 0) {
			close(sv[i*2 + 1]);
			worker(sv[i*2]);
		}
		else {
			i++;
		}
	}
    
    return 0;
}

// ------------------------------------------------------------------------  //
// ------------------------- Third part functions -------------------------  //
// ------------------------------------------------------------------------  //
int set_nonblock(int fd) {
	int flags;
#if defined(O_NONBLOCK)
	if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
		flags = 0;
	}
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
	flags = 1;
	return ioctl(fd, FIOBIO, &flags);
#endif
}

ssize_t
sock_fd_write(int sock, void *buf, ssize_t buflen, int fd)
{
    ssize_t     size;
    struct msghdr   msg;
    struct iovec    iov;
    union {
        struct cmsghdr  cmsghdr;
        char        control[CMSG_SPACE(sizeof (int))];
    } cmsgu;
    struct cmsghdr  *cmsg;

    iov.iov_base = buf;
    iov.iov_len = buflen;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (fd != -1) {
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(sizeof (int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;

        logger (logger_buf);
        *((int *) CMSG_DATA(cmsg)) = fd;
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        logger ("not passing fd\n");
    }

    size = sendmsg(sock, &msg, 0);

    if (size < 0)
        perror ("sendmsg");
    return size;
}

ssize_t
sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd)
{
    ssize_t     size;

    if (fd) {
        struct msghdr   msg;
        struct iovec    iov;
        union {
            struct cmsghdr  cmsghdr;
            char        control[CMSG_SPACE(sizeof (int))];
        } cmsgu;
        struct cmsghdr  *cmsg;

        iov.iov_base = buf;
        iov.iov_len = bufsize;

        msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);
        size = recvmsg (sock, &msg, 0);
        if (size < 0) {
            perror ("recvmsg");
            exit(1);
        }
        cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_level != SOL_SOCKET) {
                fprintf (stderr, "invalid cmsg_level %d\n",
                     cmsg->cmsg_level);
                exit(1);
            }
            if (cmsg->cmsg_type != SCM_RIGHTS) {
                fprintf (stderr, "invalid cmsg_type %d\n",
                     cmsg->cmsg_type);
                exit(1);
            }

            *fd = *((int *) CMSG_DATA(cmsg));
        } else
            *fd = -1;
    } else {
        size = read (sock, buf, bufsize);
        if (size < 0) {
            perror("read");
            exit(1);
        }
    }
    return size;
}

// ------------------------- Daemonization -------------------------  //
void skeleton_daemon()
{
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    chdir(globalArgs.dir);
	remove(log_name);

    /* Close all open file descriptors */
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close (x);
    }
}
