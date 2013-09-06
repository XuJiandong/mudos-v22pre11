/*
 *  addr_server.c -- socket-based ip address server.
 *                   8-92 : Dwayne Fontenot : original coding
 */

#include "std.h"
#include "addr_server.h"
#include "socket_ctrl.h"
#include "file_incl.h"
#include "debug.h"
#include "port.h"

#ifdef DEBUG_MACRO
int debug_level = 512;
#endif				/* DEBUG_MACRO */

/*
 * private local variables.
 */
static connection all_conns[MAX_CONNS];
static int total_conns = 0;
static queue_element_ptr queue_head = NULL;
static queue_element_ptr queue_tail = NULL;
static queue_element_ptr stack_head = NULL;
static int queue_length = 0;
static int conn_fd;

fd_set readmask;

int name_by_ip PROT((int, char *));
int ip_by_name PROT((int, char *));
INLINE_STATIC void process_queue PROT((void));
void init_conns PROT((void));
void init_conn_sock PROT((int));

#ifdef SIGNAL_FUNC_TAKES_INT
void sigpipe_handler PROT((int));

#else
void sigpipe_handler PROT((void));

#endif
INLINE void aserv_process_io PROT((int));
void enqueue_datapending PROT((int, int));
void handle_top_event PROT((void));
void dequeue_top_event PROT((void));
void pop_queue_element PROT((queue_element_ptr *));
void push_queue_element PROT((queue_element_ptr));
void new_conn_handler PROT((void));
void conn_data_handler PROT((int));
int index_by_fd PROT((int));
void terminate PROT((int));

void debug_perror P2(char *, what, char *, file) {
    if (file)
	fprintf(stderr, "System Error: %s:%s:%s\n", what, file, port_strerror(errno));
    else
	fprintf(stderr, "System Error: %s:%s\n", what, port_strerror(errno));
}

void init_conns()
{
    int i;

    for (i = 0; i < MAX_CONNS; i++) {
	all_conns[i].fd = -1;
	all_conns[i].state = CONN_CLOSED;
	all_conns[i].sname[0] = '\0';

	/* ensure 'leftover' buffer is _always_ null terminated */
	all_conns[i].buf[0] = '\0';
	all_conns[i].buf[IN_BUF_SIZE - 1] = '\0';
	all_conns[i].leftover = 0;
    }
}

/*
 * Initialize connection socket.
 */
void init_conn_sock P1(int, port_num)
{
    struct sockaddr_in sin;
    int sin_len;
    int optval;
#ifdef WINSOCK
    WSADATA WSAData;

#define CLEANUP WSACleanup()

    WSAStartup(MAKEWORD(1,1), &WSAData);
#else
#define CLEANUP
#endif

    /*
     * create socket of proper type.
     */
    if ((conn_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
	perror("init_conn_sock: socket");
	CLEANUP;
	exit(1);
    }
    /*
     * enable local address reuse.
     */
    optval = 1;
    if (setsockopt(conn_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &optval,
		   sizeof(optval)) == -1) {
	perror("init_conn_sock: setsockopt");
	CLEANUP;
	exit(2);
    }
    /*
     * fill in socket address information.
     */
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons((u_short) port_num);
    /*
     * bind name to socket.
     */
    if (bind(conn_fd, (struct sockaddr *) & sin, sizeof(sin)) == -1) {
	perror("init_conn_sock: bind");
	CLEANUP;
	exit(3);
    }
    /*
     * get socket name.
     */
    sin_len = sizeof(sin);
    if (getsockname(conn_fd, (struct sockaddr *) & sin, &sin_len) == -1) {
	perror("init_conn_sock: getsockname");
	CLEANUP;
	exit(4);
    }
    /*
     * register signal handler for SIGPIPE.
     */
#if !defined(LATTICE) && defined(SIGPIPE) /* windows has no SIGPIPE */
    if (signal(SIGPIPE, sigpipe_handler) == SIGNAL_ERROR) {
	perror("init_conn_sock: signal SIGPIPE");
	CLEANUP;
	exit(5);
    }
#endif
    /*
     * set socket non-blocking
     */
    if (set_socket_nonblocking(conn_fd, 1) == -1) {
	perror("init_user_conn: set_socket_nonblocking 1");
	CLEANUP;
	exit(8);
    }
    /*
     * listen on socket for connections.
     */
    if (listen(conn_fd, 128) == -1) {
	perror("init_conn_sock: listen");
	CLEANUP;
	exit(10);
    }
    debug(512, ("addr_server: listening for connections on port %d\n",
		port_num));
}

/*
 * SIGPIPE handler -- does very little for now.
 */
#ifdef SIGNAL_FUNC_TAKES_INT
void sigpipe_handler P1(int, sig)
{
#else
void sigpipe_handler()
{
#endif
    fprintf(stderr, "SIGPIPE received.\n");
}

/*
 * I/O handler.
 */
INLINE void aserv_process_io P1(int, nb)
{
    int i;

    switch (nb) {
    case -1:
	perror("sigio_handler: select");
	break;
    case 0:
	break;
    default:
	/*
	 * check for new connection.
	 */
	if (FD_ISSET(conn_fd, &readmask)) {
	    debug(512, ("sigio_handler: NEW_CONN\n"));
	    enqueue_datapending(conn_fd, NEW_CONN);
	}
	/*
	 * check for data pending on established connections.
	 */
	for (i = 0; i < MAX_CONNS; i++) {
	    if (FD_ISSET(all_conns[i].fd, &readmask)) {
		debug(512, ("sigio_handler: CONN\n"));
		enqueue_datapending(all_conns[i].fd, CONN);
	    }
	}
	break;
    }
}

INLINE_STATIC void process_queue()
{
    int i;

    for (i = 0; queue_head && (i < MAX_EVENTS_TO_PROCESS); i++) {
	handle_top_event();
	dequeue_top_event();
    }
}

void enqueue_datapending P2(int, fd, int, fd_type)
{
    queue_element_ptr new_queue_element;

    pop_queue_element(&new_queue_element);
    new_queue_element->event_type = fd_type;
    new_queue_element->fd = fd;
    new_queue_element->next = NULL;
    if (queue_head) {
	queue_tail->next = new_queue_element;
    } else {
	queue_head = new_queue_element;
    }
    queue_tail = new_queue_element;
}

void dequeue_top_event()
{
    queue_element_ptr top_queue_element;

    if (queue_head) {
	top_queue_element = queue_head;
	queue_head = queue_head->next;
	push_queue_element(top_queue_element);
    } else {
	fprintf(stderr, "dequeue_top_event: tried to dequeue from empty queue!\n");
    }
}

void pop_queue_element P1(queue_element_ptr *, the_queue_element)
{
    if ((*the_queue_element = stack_head))
	stack_head = stack_head->next;
    else
	*the_queue_element = (queue_element_ptr) malloc(sizeof(queue_element));
    queue_length++;
}

void push_queue_element P1(queue_element_ptr, the_queue_element)
{
    the_queue_element->next = stack_head;
    stack_head = the_queue_element;
    queue_length--;
}

void handle_top_event()
{
    switch (queue_head->event_type) {
	case NEW_CONN:
	debug(512, ("handle_top_event: NEW_CONN\n"));
	new_conn_handler();
	break;
    case CONN:
	debug(512, ("handle_top_event: CONN data on fd %d\n", queue_head->fd));
	conn_data_handler(queue_head->fd);
	break;
    default:
	fprintf(stderr, "handle_top_event: unknown event type %d\n",
		queue_head->event_type);
	break;
    }
}

/*
 * This is the new connection handler. This function is called by the
 * event handler when data is pending on the listening socket (conn_fd).
 * If space is available, an interactive data structure is initialized and
 * the connected is established.
 */
void new_conn_handler()
{
    struct sockaddr_in client;
    int client_len;
    struct hostent *c_hostent;
    int new_fd;
    int conn_index;

    client_len = sizeof(client);
    new_fd = accept(conn_fd, (struct sockaddr *) & client, (int *) &client_len);
    if (new_fd == -1) {
	perror("new_conn_handler: accept");
	return;
    }
    if (total_conns >= MAX_CONNS) {
	char *message = "no available slots -- closing connection.\n";

	fprintf(stderr, "new_conn_handler: no available connection slots.\n");
	OS_socket_write(new_fd, message, strlen(message));
	if (OS_socket_close(new_fd) == -1)
	    perror("new_conn_handler: close");
	return;
    }
    /* get some information about new connection */
    c_hostent = gethostbyaddr((char *) &client.sin_addr.s_addr,
			      sizeof(client.sin_addr.s_addr), AF_INET);
    for (conn_index = 0; conn_index < MAX_CONNS; conn_index++) {
	if (all_conns[conn_index].state == CONN_CLOSED) {
	    debug(512, ("new_conn_handler: opening conn index %d\n", conn_index));
	    /* update global data for new fd */
	    all_conns[conn_index].fd = new_fd;
	    all_conns[conn_index].state = CONN_OPEN;
	    all_conns[conn_index].addr = client;
	    if (c_hostent)
		strcpy(all_conns[conn_index].sname, c_hostent->h_name);
	    else
		strcpy(all_conns[conn_index].sname, "<unknown>");
	    total_conns++;
	    return;
	}
    }
    fprintf(stderr, "new_conn_handler: sanity check failed!\n");
}

void conn_data_handler P1(int, fd)
{
    int conn_index;
    int buf_index;
    int num_bytes;
    int msgtype;
    int leftover;
    char *buf;
    int res, msglen, expecting;
    long unread_bytes;

    if ((conn_index = index_by_fd(fd)) == -1) {
	fprintf(stderr, "conn_data_handler: invalid fd.\n");
	return;
    }
    debug(512, ("conn_data_handler: read on fd %d\n", fd));

    /* append new data to end of leftover data (if any) */
    leftover = all_conns[conn_index].leftover;
    buf = (char *) &all_conns[conn_index].buf[0];
    num_bytes = OS_socket_read(fd, buf + leftover, (IN_BUF_SIZE - 1) - leftover);

    switch (num_bytes) {
    case -1:
	switch (errno) {
#ifdef WIN32
	case WSAEWOULDBLOCK:
#endif
	case EWOULDBLOCK:
	    debug(512, ("conn_data_handler: read on fd %d: Operation would block.\n",
			fd));
	    break;
	default:
	    perror("conn_data_handler: read");
	    terminate(conn_index);
	    break;
	}
	break;
    case 0:
	if (all_conns[conn_index].state == CONN_CLOSED)
	    fprintf(stderr, "get_user_data: tried to read from closed fd.\n");
	terminate(conn_index);
	break;
    default:
	debug(512, ("conn_data_handler: read %d bytes on fd %d\n", num_bytes, fd));
	num_bytes += leftover;
	buf_index = 0;
	expecting = 0;
	while (num_bytes > sizeof(int) && buf_index < (IN_BUF_SIZE - 1)) {
	    /* get message type */
	    memcpy((char *) &msgtype, (char *) &buf[buf_index], sizeof(int));
	    debug(512, ("conn_data_handler: message type: %d\n", msgtype));

	    if (msgtype == NAMEBYIP) {
		if (buf[buf_index + sizeof(int)] == '\0') {
		    /* no data here...resync */
		    buf_index++;
		    num_bytes--;
		    continue;
		}
		if (expecting && num_bytes < expecting) {
		    /*
		     * message truncated...back up to DATALEN message; exit
		     * loop...and try again later
		     */
		    buf_index -= (sizeof(int) + sizeof(int));
		    num_bytes += (sizeof(int) + sizeof(int));
		    break;
		}
		res = name_by_ip(conn_index, &buf[buf_index]);
	    } else if (msgtype == IPBYNAME) {
		if (buf[buf_index + sizeof(int)] == '\0') {
		    /* no data here...resync */
		    buf_index++;
		    num_bytes--;
		    continue;
		}
		if (expecting && num_bytes < expecting) {
		    /*
		     * message truncated...back up to DATALEN message; exit
		     * loop...and try again later
		     */
		    buf_index -= (sizeof(int) + sizeof(int));
		    num_bytes += (sizeof(int) + sizeof(int));
		    break;
		}
		res = ip_by_name(conn_index, &buf[buf_index]);
	    } else if (msgtype == DATALEN) {
		if (num_bytes > (sizeof(int) + sizeof(int))) {
		    memcpy((char *) &expecting, (char *) &buf[buf_index + sizeof(int)], sizeof(int));
		    /*
		     * advance to next message
		     */
		    buf_index += (sizeof(int) + sizeof(int));
		    num_bytes -= (sizeof(int) + sizeof(int));
		    if (expecting > IN_BUF_SIZE || expecting <= 0) {
			fprintf(stderr, "conn_data_handler: bad data length %d\n", expecting);
			expecting = 0;
		    }
		    continue;
		} else {
		    /*
		     * not enough bytes...assume truncated; exit loop...we'll
		     * handle this message later
		     */
		    break;
		}
	    } else {
		fprintf(stderr, "conn_data_handler: unknown message type %08x\n", msgtype);
		/* advance through buffer */
		buf_index++;
		num_bytes--;
		continue;
	    }

	    msglen = (int) (sizeof(int) + strlen(&buf[buf_index + sizeof(int)]) +1);
	    if (res) {
		/*
		 * ok...advance to next message
		 */
		buf_index += msglen;
		num_bytes -= msglen;
	    } else if (msglen < num_bytes || (expecting && expecting == msglen)) {
		/*
		 * failed...
		 */

		/*
		 * this was a complete message...advance to the next one
		 */
		msglen = (int) (sizeof(int) + strlen(&buf[buf_index + sizeof(int)]) +1);
		buf_index += msglen;
		num_bytes -= msglen;
		expecting = 0;
	    }
	    else if (!OS_socket_ioctl(fd, FIONREAD, &unread_bytes) &&
		     unread_bytes > 0) {
		/*
		 * msglen == num_bytes could be a complete message... if
		 * there's unread data we'll assume it was truncated
		 */
		break;
	    } else {
		/*
		 * nothing more?  then discard message (it was the last one)
		 */
		buf_index = 0;
		num_bytes = 0;
		break;
	    }
	}

	/* keep track of leftover buffer contents */
	if (num_bytes && buf_index)
	    memmove(buf, &buf[buf_index], num_bytes);
	buf[num_bytes] = '\0';
	all_conns[conn_index].leftover = num_bytes;

	break;
    }
}

#define OUT_BUF_SIZE 80

int ip_by_name P2(int, conn_index, char *, buf)
{
    struct hostent *hp;
    struct in_addr my_in_addr;
    static char out_buf[OUT_BUF_SIZE];

    hp = gethostbyname(&buf[sizeof(int)]);
    if (hp == NULL) {
/* Failed :( */
	sprintf(out_buf, "%s %s\n", &buf[sizeof(int)], "0");
	debug(512, ("%s", out_buf));
	OS_socket_write(all_conns[conn_index].fd, out_buf, strlen(out_buf));
	return 0;
    } else {
/* Success! */
	memcpy(&my_in_addr, hp->h_addr, sizeof(struct in_addr));
	sprintf(out_buf, "%s %s\n", &buf[sizeof(int)],
		inet_ntoa(my_in_addr));
	debug(512, ("%s", out_buf));
	OS_socket_write(all_conns[conn_index].fd, out_buf, strlen(out_buf));
	return 1;
    }
}				/* ip_by_name() */

int name_by_ip P2(int, conn_index, char *, buf)
{
    long addr;
    struct hostent *hp;
    static char out_buf[OUT_BUF_SIZE];

    if ((addr = inet_addr(&buf[sizeof(int)])) == -1) {
	debug(512, ("name_by_ip: malformed address request.\n"));
	return 0;
    }
    if ((hp = gethostbyaddr((char *) &addr, sizeof(addr), AF_INET))) {
	sprintf(out_buf, "%s %s\n", &buf[sizeof(int)], hp->h_name);
	debug(512, ("%s", out_buf));
	OS_socket_write(all_conns[conn_index].fd, out_buf, strlen(out_buf));
	return 1;
    } else {
	sprintf(out_buf, "%s 0\n", &buf[sizeof(int)]);
	debug(512, ("%s", out_buf));
	OS_socket_write(all_conns[conn_index].fd, out_buf, strlen(out_buf));
	debug(512, ("name_by_ip: unable to resolve address.\n"));
	return 0;
    }
}

int index_by_fd P1(int, fd)
{
    int i;

    for (i = 0; i < MAX_CONNS; i++) {
	if ((all_conns[i].state == CONN_OPEN) && (all_conns[i].fd == fd))
	    return (i);
    }
    return (-1);
}

void terminate P1(int, conn_index)
{
    if (conn_index < 0 || conn_index >= MAX_CONNS) {
	fprintf(stderr, "terminate: conn_index %d out of range.\n", conn_index);
	return;
    }
    if (all_conns[conn_index].state == CONN_CLOSED) {
	fprintf(stderr, "terminate: connection %d already closed.\n", conn_index);
	return;
    }
    debug(512, ("terminating connection %d\n", conn_index));

    if (OS_socket_close(all_conns[conn_index].fd) == -1) {
	perror("terminate: close");
	return;
    }
    all_conns[conn_index].state = CONN_CLOSED;
    total_conns--;
}

int main(argc, argv)
    int argc;
    char *argv[];
{
    int addr_server_port;
    struct timeval timeout;
    int i;
    int nb;

    if (argc > 1) {
	if ((addr_server_port = atoi(argv[1])) == 0) {
	    fprintf(stderr, "addr_server: malformed port number.\n");
	    exit(2);
	}
    } else {
	fprintf(stderr, "addr_server: first arg must be port number.\n");
	exit(1);
    }
#if defined(LATTICE) && defined(AMITCP)
    init_conns();
#endif
    init_conn_sock(addr_server_port);
    while (1) {
	/*
	 * use finite timeout for robustness.
	 */
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;
	/*
	 * clear selectmasks.
	 */
	FD_ZERO(&readmask);
	/*
	 * set new connection accept fd in readmask.
	 */
	FD_SET(conn_fd, &readmask);
	/*
	 * set active fds in readmask.
	 */
	for (i = 0; i < MAX_CONNS; i++) {
	    if (all_conns[i].state == CONN_OPEN)
		FD_SET(all_conns[i].fd, &readmask);
	}
#ifndef hpux
	nb = select(FD_SETSIZE, &readmask, (fd_set *) 0, (fd_set *) 0, &timeout);
#else
	nb = select(FD_SETSIZE, (int *) &readmask, (int *) 0, (int *) 0, &timeout);
#endif
	if (nb != 0)
	    aserv_process_io(nb);
	process_queue();
    }
    /* the following is to shut lint up */
    /*NOTREACHED*/
    return 0;			/* never reached */
}
