/*
 *  comm.c -- communications functions and more.
 *            Dwayne Fontenot (Jacques@TMI)
 *  Windows 95 support by George Reese (Descartes of Borg)
 */
#include "std.h"
#include "network_incl.h"
#include "lpc_incl.h"
#include "applies.h"
#include "main.h"
#include "comm.h"
#include "socket_efuns.h"
#include "backend.h"
#include "socket_ctrl.h"
#include "eoperators.h"
#include "debug.h"
#include "ed.h"
#include "file.h"

#define TELOPTS

int total_users = 0;

/*
 * local function prototypes.
 */
static int copy_chars PROT((unsigned char *, unsigned char *, int, interactive_t *));

#ifdef SIGNAL_FUNC_TAKES_INT
static void sigpipe_handler PROT((int));
#else
static void sigpipe_handler PROT((void));
#endif
static void hname_handler PROT((void));
static void get_user_data PROT((interactive_t *));
static char *get_user_command PROT((void));
static char *first_cmd_in_buf PROT((interactive_t *));
static int cmd_in_buf PROT((interactive_t *));
static void next_cmd_in_buf PROT((interactive_t *));
static int call_function_interactive PROT((interactive_t *, char *));
static void print_prompt PROT((interactive_t *));
static void telnet_neg PROT((char *, char *));
static void query_addr_name PROT((object_t *));
static void got_addr_number PROT((char *, char *));
static void add_ip_entry PROT((long, char *));
#ifndef NO_ADD_ACTION
static void clear_notify PROT((interactive_t *));
#endif
static void new_user_handler PROT((int));
static void receive_snoop PROT((char *, object_t * ob));

/*
 * public local variables.
 */
fd_set readmask, writemask;
int num_user;
int num_hidden;			/* for the O_HIDDEN flag.  This counter must
				 * be kept up to date at all times!  If you
				 * modify the O_HIDDEN flag in an object,
				 * make sure that you update this counter if
				 * the object is interactive. */
int add_message_calls = 0;
int inet_packets = 0;
int inet_volume = 0;
interactive_t **all_users = 0;
int max_users = 0;

/*
 * private local variables.
 */
static int addr_server_fd = -1;

static void
receive_snoop P2(char *, buf, object_t *, snooper)
{
/* command giver no longer set to snooper */
#ifdef RECEIVE_SNOOP
    copy_and_push_string(buf);
    apply(APPLY_RECEIVE_SNOOP, snooper, 1, ORIGIN_DRIVER);
#else
    /* snoop output is now % in all cases */
    add_message(snooper, "%");
    add_message(snooper, buf);
#endif
}

/*
 * Initialize new user connection socket.
 */
void init_user_conn()
{
    struct sockaddr_in sin;
    int sin_len;
    int optval;
    int i;

#ifdef WINSOCK
    WSADATA WSAData;

#define CLEANUP WSACleanup()
    
    WSAStartup(MAKEWORD(1, 1), &WSAData);
#else
#define CLEANUP
#endif

    for (i=0; i < 5; i++) {
	if (!external_port[i].port) continue;
	/*
	 * create socket of proper type.
	 */
	if ((external_port[i].fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
	    debug_perror("init_user_conn: socket", 0);
	    exit(1);
	}
	/*
	 * enable local address reuse.
	 */
	optval = 1;
	if (setsockopt(external_port[i].fd, SOL_SOCKET, SO_REUSEADDR,
		       (char *) &optval, sizeof(optval)) == -1) {
	    debug_perror("init_user_conn: setsockopt", 0);
	    CLEANUP;
	    exit(2);
	}
	/*
	 * fill in socket address information.
	 */
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
	sin.sin_port = htons((u_short) external_port[i].port);
	/*
	 * bind name to socket.
	 */
	if (bind(external_port[i].fd, (struct sockaddr *) & sin,
		 sizeof(sin)) == -1) {
	    debug_perror("init_user_conn: bind", 0);
	    CLEANUP;
	    exit(3);
	}
	/*
	 * get socket name.
	 */
	sin_len = sizeof(sin);
	if (getsockname(external_port[i].fd, (struct sockaddr *) & sin,
			&sin_len) == -1) {
	    debug_perror("init_user_conn: getsockname", 0);
	    CLEANUP;
	    exit(4);
	}
	/*
	 * set socket non-blocking,
	 */
	if (set_socket_nonblocking(external_port[i].fd, 1) == -1) {
	    debug_perror("init_user_conn: set_socket_nonblocking 1", 0);
	    CLEANUP;
	    exit(8);
	}
	/*
	 * listen on socket for connections.
	 */
	if (listen(external_port[i].fd, 128) == -1) {
	    debug_perror("init_user_conn: listen", 0);
	    CLEANUP;
	    exit(10);
	}
    }
    /*
     * register signal handler for SIGPIPE.
     */
#if !defined(LATTICE) && defined(SIGPIPE)
    if (signal(SIGPIPE, sigpipe_handler) == SIGNAL_ERROR) {
	debug_perror("init_user_conn: signal SIGPIPE",0);
	exit(5);
    }
#endif
}

/*
 * Shut down new user accept file descriptor.
 */
void ipc_remove()
{
    int i;

    for (i = 0; i < 5; i++) {
	if (!external_port[i].port) continue;
	if (OS_socket_close(external_port[i].fd) == -1) {
	    debug_perror("ipc_remove: close", 0);
	}
    }

    debug_message("closed external ports\n");
}

void init_addr_server P2(char *, hostname, int, addr_server_port)
{
    struct sockaddr_in server;
    struct hostent *hp;
    int server_fd;
    int optval;
    long addr;

    if (!hostname) return;

    /*
     * get network host data for hostname.
     */
    if (hostname[0] >= '0' && hostname[0] <= '9' &&
          (addr = inet_addr(&hostname[0])) != -1) {
        hp = gethostbyaddr((char *)&addr, sizeof(addr), AF_INET);
    } else {
        hp = gethostbyname(hostname);
    }
    if (hp == NULL) {
	debug_perror("init_addr_server: gethostbyname", 0);
	return;
    }

    /*
     * set up address information for server.
     */
    server.sin_family = AF_INET;
    server.sin_port = htons((u_short) addr_server_port);
    server.sin_addr.s_addr = inet_addr(hostname);
    memcpy((char *) &server.sin_addr, (char *) hp->h_addr, hp->h_length);
    /*
     * create socket of proper type.
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {	/* problem opening socket */
	debug_perror("init_addr_server: socket", 0);
	return;
    }
    /*
     * enable local address reuse.
     */
    optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &optval,
		   sizeof(optval)) == -1) {
	debug_perror("init_addr_server: setsockopt", 0);
	return;
    }
    /*
     * connect socket to server address.
     */
    if (connect(server_fd, (struct sockaddr *) & server, sizeof(server)) == -1) {
#ifdef WINSOCK
	if (errno == WSAECONNREFUSED)
#else
	if (errno == ECONNREFUSED)
#endif
	    debug_message("Connection to address server (%s %d) refused.\n",
			  hostname, addr_server_port);
	else 
	    debug_perror("init_addr_server: connect", 0);
	OS_socket_close(server_fd);
	return;
    }
    addr_server_fd = server_fd;
    debug_message("Connected to address server on %s port %d\n", hostname,
	    addr_server_port);
    /*
     * set socket non-blocking.
     */
    if (set_socket_nonblocking(server_fd, 1) == -1) {
	debug_perror("init_addr_server: set_socket_nonblocking 1", 0);
	return;
    }
}

/*
 * If there is a shadow for this object, then the message should be
 * sent to it. But only if catch_tell() is defined. Beware that one of the
 * shadows may be the originator of the message, which means that we must
 * not send the message to that shadow, or any shadows in the linked list
 * before that shadow.
 *
 * Also note that we don't need to do this in the case of
 * INTERACTIVE_CATCH_TELL, since catch_tell() was already called
 * _instead of_ add_message(), and shadows got their chance then.
 */
#if !defined(INTERACTIVE_CATCH_TELL) && !defined(NO_SHADOWS)
#define SHADOW_CATCH_MESSAGE
#endif

#ifdef SHADOW_CATCH_MESSAGE
static int shadow_catch_message P2(object_t *, ob, char *, str)
{
    if (!ob->shadowed)
	return 0;
    while (ob->shadowed != 0 && ob->shadowed != current_object)
	ob = ob->shadowed;
    while (ob->shadowing) {
	copy_and_push_string(str);
	if (apply(APPLY_CATCH_TELL, ob, 1, ORIGIN_DRIVER))	
	    /* this will work, since we know the */
	    /* function is defined */
	    return 1;
	ob = ob->shadowing;
    }
    return 0;
}
#endif

/*
 * Send a message to an interactive object. If that object is shadowed,
 * special handling is done.
 */
void add_message P2(object_t *, who, char *, data)
{
    interactive_t *ip;
    char *cp;

    /*
     * if who->interactive is not valid, write message on stderr.
     * (maybe)
     */
    if (!who || (who->flags & O_DESTRUCTED) || !who->interactive ||
	(who->interactive->iflags & (NET_DEAD | CLOSING))) {
#ifdef NONINTERACTIVE_STDERR_WRITE
	putc(']', stderr);
	fputs(data, stderr);
#endif
#ifdef LATTICE
	fflush(stderr);
#endif
	return;
    }
    ip = who->interactive;
#ifdef SHADOW_CATCH_MESSAGE
    /*
     * shadow handling.
     */
    if (shadow_catch_message(who, data)) {
	/*
	 * snoop handling.
	 */
#ifdef SNOOP_SHADOWED
	if (ip->snoop_by) 
	    receive_snoop(data, ip->snoop_by->ob);
#endif
	return;
    }
#endif				/* NO_SHADOWS */

    /*
     * write message into ip->message_buf.
     */
    for (cp = data; *cp != '\0'; cp++) {
	if (ip->message_length == MESSAGE_BUF_SIZE) {
	    if (!flush_message(ip)) {
		debug_message("Broken connection during add_message.\n");
		return;
	    }
	    if (ip->message_length == MESSAGE_BUF_SIZE)
		break;
	}
	if (*cp == '\n') {
	    if (ip->message_length == (MESSAGE_BUF_SIZE - 1)) {
		if (!flush_message(ip)) {
		    debug_message("Broken connection during add_message.\n");
		    return;
		}
		if (ip->message_length == (MESSAGE_BUF_SIZE - 1))
		    break;
	    }
	    ip->message_buf[ip->message_producer] = '\r';
	    ip->message_producer = (ip->message_producer + 1)
		% MESSAGE_BUF_SIZE;
	    ip->message_length++;
	}
	ip->message_buf[ip->message_producer] = *cp;
	ip->message_producer = (ip->message_producer + 1) % MESSAGE_BUF_SIZE;
	ip->message_length++;
    }
    /*
     * snoop handling.
     */
    if (ip->snoop_by)
	receive_snoop(data, ip->snoop_by->ob);

#ifdef FLUSH_OUTPUT_IMMEDIATELY
    flush_message(ip);
#endif
    
    add_message_calls++;
}				/* add_message() */

/* WARNING: this can only handle results <= LARGEST_PRINTABLE_STRING in size */
void add_vmessage P2V(object_t *, who, char *, format)
{
    interactive_t *ip;
    char *cp, new_string_data[LARGEST_PRINTABLE_STRING + 1];
    va_list args;
    V_DCL(char *format);
    V_DCL(object_t *who);

    V_START(args, format);
    V_VAR(object_t *, who, args);
    V_VAR(char *, format, args);
    /*
     * if who->interactive is not valid, write message on stderr.
     * (maybe)
     */
    if (!who || (who->flags & O_DESTRUCTED) || !who->interactive || 
	(who->interactive->iflags & (NET_DEAD | CLOSING))) {
#ifdef NONINTERACTIVE_STDERR_WRITE
	putc(']', stderr);
	vfprintf(stderr, format, args);
#endif
	va_end(args);
#ifdef LATTICE
	fflush(stderr);
#endif
	return;
    }
    ip = who->interactive;
    new_string_data[0] = '\0';
    /*
     * this is dangerous since the data may not all fit into new_string_data
     * but how to tell if it will without trying it first?  I suppose one
     * could rewrite vsprintf to accept a maximum length (like strncpy) --
     * have fun!
     */
    vsprintf(new_string_data, format, args);
    va_end(args);
#ifdef SHADOW_CATCH_MESSAGE
    /*
     * shadow handling.
     */
    if (shadow_catch_message(who, new_string_data)) {
	/*
	 * snoop handling.
	 */
#ifdef SNOOP_SHADOWED
	if (ip->snoop_by)
	    receive_snoop(new_string_data, ip->snoop_by->ob);
#endif
	return;
    }
#endif				/* NO_SHADOWS */

    /*
     * write message into ip->message_buf.
     */
    for (cp = new_string_data; *cp != '\0'; cp++) {
	if (ip->message_length == MESSAGE_BUF_SIZE) {
	    if (!flush_message(ip)) {
		debug_message("Broken connection during add_message.\n");
		return;
	    }
	    if (ip->message_length == MESSAGE_BUF_SIZE)
		break;
	}
	if (*cp == '\n') {
	    if (ip->message_length == (MESSAGE_BUF_SIZE - 1)) {
		if (!flush_message(ip)) {
		    debug_message("Broken connection during add_message.\n");
		    return;
		}
		if (ip->message_length == (MESSAGE_BUF_SIZE - 1))
		    break;
	    }
	    ip->message_buf[ip->message_producer] = '\r';
	    ip->message_producer = (ip->message_producer + 1)
		% MESSAGE_BUF_SIZE;
	    ip->message_length++;
	}
	ip->message_buf[ip->message_producer] = *cp;
	ip->message_producer = (ip->message_producer + 1) % MESSAGE_BUF_SIZE;
	ip->message_length++;
    }
    if (ip->message_length != 0) {
	if (!flush_message(ip)) {
	    debug_message("Broken connection during add_message.\n");
	    return;
	}
    }
    /*
     * snoop handling.
     */
    if (ip->snoop_by)
	receive_snoop(new_string_data, ip->snoop_by->ob);

#ifdef FLUSH_OUTPUT_IMMEDIATELY
    flush_message(ip);
#endif
    
    add_message_calls++;
}				/* add_message() */

/*
 * Flush outgoing message buffer of current interactive object.
 */
int flush_message P1(interactive_t *, ip)
{
    int length, num_bytes;

    /*
     * if ip is not valid, do nothing.
     */
    if (!ip || (ip->iflags & CLOSING)) {
	debug_message("flush_message: invalid target!\n");
	return 0;
    }
    /*
     * write ip->message_buf[] to socket.
     */
    while (ip->message_length != 0) {
	if (ip->message_consumer < ip->message_producer) {
	    length = ip->message_producer - ip->message_consumer;
	} else {
	    length = MESSAGE_BUF_SIZE - ip->message_consumer;
	}
/* Need to use send to get out of band data
   num_bytes = write(ip->fd,ip->message_buf + ip->message_consumer,length);
 */
	num_bytes = send(ip->fd, ip->message_buf + ip->message_consumer,
			 length, ip->out_of_band);
	if (num_bytes == -1) {
#ifdef EWOULDBLOCK
	    if (errno == EWOULDBLOCK) {
		debug(512, ("flush_message: write: Operation would block\n"));
		return 1;
#else
#  ifdef WINSOCK
	    if (errno == WSAEWOULDBLOCK) {
		debug(512, ("flush_message: write: Operation would block\n"));
		return 1;
#  else
	    if (0) {
		;
#  endif
#endif
#ifdef linux
	    } else if (errno == EINTR) {
		debug(512, ("flush_message: write: Interrupted system call"));
		return 1;
#endif
	    } else {
		debug_perror("flush_message: write", 0);
		ip->iflags |= NET_DEAD;
		return 0;
	    }
	}
	ip->message_consumer = (ip->message_consumer + num_bytes) %
	    MESSAGE_BUF_SIZE;
	ip->message_length -= num_bytes;
	ip->out_of_band = 0;
	inet_packets++;
	inet_volume += num_bytes;
    }
    return 1;
}				/* flush_message() */

#define TS_DATA         0
#define TS_IAC          1
#define TS_WILL         2
#define TS_WONT         3
#define TS_DO           4
#define TS_DONT         5
#define TS_SB		6
#define TS_SB_IAC       7
/*
 * Copy a string, replacing newlines with '\0'. Also add an extra
 * space and back space for every newline. This trick will allow
 * otherwise empty lines, as multiple newlines would be replaced by
 * multiple zeroes only.
 *
 * Also handle the telnet stuff.  So instead of this being a direct
 * copy it is a small state thingy.
 *
 * In fact, it is telnet_neg conglomerated into this.  This is mostly
 * done so we can sanely remove the telnet sub option negotation stuff
 * out of the input stream.  Need this for terminal types.
 * (Pinkfish change)
 */
/* the codes we send ... */
#define SCHAR SIGNED char

static char telnet_break_response[] = 
    { 28, (SCHAR)IAC, (SCHAR)WILL, TELOPT_TM, 0 };
static char telnet_interrupt_response[] = 
    { 127, (SCHAR)IAC, (SCHAR)WILL, TELOPT_TM, 0 };
static char telnet_abort_response[] = 
    { (SCHAR)IAC, (SCHAR)DM, 0 };
static char telnet_do_tm_response[] = 
    { (SCHAR)IAC, (SCHAR)WILL, TELOPT_TM, 0 };
static char telnet_do_naws[] =
    { (SCHAR)IAC, (SCHAR)DO, TELOPT_NAWS, 0 };
static char telnet_do_ttype[] =
    { (SCHAR)IAC, (SCHAR)DO, TELOPT_TTYPE, 0 };
static char telnet_term_query[] = 
    { (SCHAR)IAC, (SCHAR)SB, TELOPT_TTYPE, TELQUAL_SEND, 
	  (SCHAR)IAC, (SCHAR)SE, 0 };
static char telnet_no_echo[] = 
    { (SCHAR)IAC, (SCHAR)WONT, TELOPT_ECHO, 0 };
static char telnet_no_single[] = 
    { (SCHAR)IAC, (SCHAR)WONT, TELOPT_SGA, 0 };
static char telnet_yes_echo[] = 
    { (SCHAR)IAC, (SCHAR)WILL, TELOPT_ECHO, 0 };
static char telnet_yes_single[] = 
    { (SCHAR)IAC, (SCHAR)WILL, TELOPT_SGA, 0 };
static char telnet_ga[] = 
    { (SCHAR)IAC, (SCHAR)GA, 0 };

static int copy_chars P4(unsigned char *, from, unsigned char *, to, int, n, interactive_t *, ip)
{
    int i;
    unsigned char *start = to;

    for (i = 0; i < n; i++) {
	switch (ip->state) {
	case TS_DATA:
	    switch (from[i]) {
	    case IAC:
		ip->state = TS_IAC;
		break;
	    case '\r':
		if (ip->iflags & SINGLE_CHAR)
		    *to++ = from[i];
		break;
	    case '\n':
		if (ip->iflags & SINGLE_CHAR)
		    *to++ = from[i];
		else {
		    *to++ = ' ';
		    *to++ = '\b';
		    *to++ = '\0';
		}
		break;
	    default:
		*to++ = from[i];
		break;
	    }
	    break;
	case TS_SB_IAC:
	    if (from[i] == IAC) {
		if (ip->sb_pos >= SB_SIZE)
		    break;
		/* IAC IAC is a quoted IAC char */
		ip->sb_buf[ip->sb_pos++] = (SCHAR)IAC;
		ip->state = TS_SB;
		break;
	    }
	    /* SE counts as going back into data mode */
	    if (from[i] == SE) {
		/*
		 * Ok...  need to call a function on the interactive object,
		 * passing the buffer as a paramater.
		 */
		ip->sb_buf[ip->sb_pos] = 0;
		if (ip->sb_buf[0]==TELOPT_TTYPE && ip->sb_buf[1]=='I') {
		    /* TELOPT_TTYPE TELQUAL_IS means it's telling us it's
		       terminal type */
		    copy_and_push_string(ip->sb_buf + 2);
		    apply(APPLY_TERMINAL_TYPE, ip->ob, 1, ORIGIN_DRIVER);
		} else
		if (ip->sb_buf[0]==TELOPT_NAWS) {
		    int w, h, i, c[4];
		    
		    /* (char)0 is stored as 'I'; convert back */
		    for (i = 0; i < 4; i++)
			c[i] = (ip->sb_buf[i+1] == 'I' ? 0 : ip->sb_buf[i+1]);
		    
		    w = ((unsigned char)c[0]) * 256
			+ ((unsigned char)c[1]);
		    h = ((unsigned char)c[2]) * 256
			+ ((unsigned char)c[3]);
		    push_number(w);
		    push_number(h);
		    apply(APPLY_WINDOW_SIZE, ip->ob, 2, ORIGIN_DRIVER);
		} else {
		    copy_and_push_string(ip->sb_buf);
		    apply(APPLY_TELNET_SUBOPTION, ip->ob, 1, ORIGIN_DRIVER);
		}
		ip->state = TS_DATA;
		break;
	    }
	    /* old MudOS treated IAC during IAC SB ... IAC SE as return
	     * to data mode.  This is another protocol violation, I think...
	     * Fall through and handle it as if we were in data mode.
	     */
	case TS_IAC:
	    switch (from[i]) {
	    case IAC:
		/* IAC IAC is a quoted IAC char */
		*to++ = IAC;
		ip->state = TS_DATA;
		break;
	    case DO:
		ip->state = TS_DO;
		break;
	    case DONT:
		ip->state = TS_DONT;
		break;
	    case WILL:
		ip->state = TS_WILL;
		break;
	    case WONT:
		ip->state = TS_WONT;
		break;
	    case BREAK:
/* Send back a break character. */
		add_message(ip->ob, telnet_break_response);
		flush_message(ip);
		break;
	    case IP:
/* Send back an interupt process character. */
		add_message(ip->ob, telnet_interrupt_response);
		break;
	    case AYT:
/* Are you there signal.  Yep we are. */
		add_message(ip->ob, "\n[-Yes-] \n");
		break;
	    case AO:
/* Abort output. Do a telnet sync operation. */
		ip->out_of_band = MSG_OOB;
		add_message(ip->ob, telnet_abort_response);
		flush_message(ip);
		break;
	    case SB:
		ip->state = TS_SB;
		ip->sb_pos = 0;
		break;
	    case SE:
		/* IAC SE with no matching IAC SB; protocol violation, ignore
		   it */
	    case DM:
	    default:
		ip->state = TS_DATA;
		break;
	    }
	    break;
	case TS_DO:
	    if (from[i] == TELOPT_TM) {
		add_message(ip->ob, telnet_do_tm_response);
		flush_message(ip);
	    }
	    ip->state = TS_DATA;
	    break;
	case TS_WILL:
	    /* if we get any IAC WILL or IAC WONTs back, we assume they
	     * understand the telnet protocol.  Typically this will become
	     * set at the first IAC WILL/WONT TTYPE/NAWS response to the
	     * initial queries.
	     */
	    ip->iflags |= USING_TELNET;

	    if (from[i] == TELOPT_TTYPE) {
		add_message(ip->ob, telnet_term_query);
		flush_message(ip);
	    }
	case TS_DONT:
	case TS_WONT:
	    /* if we get any IAC WILL or IAC WONTs back, we assume they
	     * understand the telnet protocol.  Typically this will become
	     * set at the first IAC WILL/WONT TTYPE/NAWS response to the
	     * initial queries.
	     */
	    ip->iflags |= USING_TELNET;
	    ip->state = TS_DATA;
	    break;
	case TS_SB:
	    if ((unsigned char) from[i] == IAC) {
		ip->state = TS_SB_IAC;
		break;
	    }
/* Ok, put all the suboption stuff into the buffer on the interactive */
	    if (ip->sb_pos >= SB_SIZE)
		break;		/* Ignore stuff outside the range */
	    if (from[i])
		ip->sb_buf[ip->sb_pos++] = from[i];
	    else
		ip->sb_buf[ip->sb_pos++] = 'I';	/* Turn 0's into I's */
	    break;
	}
    }
    return (to - start);
}				/* copy_chars() */

/*
 * SIGPIPE handler -- does very little for now.
 */
#ifndef WIN32
#ifdef SIGNAL_FUNC_TAKES_INT
static void sigpipe_handler P1(int, sig)
#else
static void sigpipe_handler()
#endif
{
    debug_message("SIGPIPE received.\n");
    signal(SIGPIPE, sigpipe_handler);
}				/* sigpipe_handler() */

/*
 * SIGALRM handler.
 */
#ifdef SIGNAL_FUNC_TAKES_INT
void sigalrm_handler P1(int, sig)
#else
void sigalrm_handler()
#endif
{
    heart_beat_flag = 1;
    debug(512, ("sigalrm_handler: SIGALRM\n"));
}				/* sigalrm_handler() */
#endif

INLINE void make_selectmasks()
{
    int i;

    /*
     * generate readmask and writemask for select() call.
     */
    FD_ZERO(&readmask);
    FD_ZERO(&writemask);
    /*
     * set new user accept fd in readmask.
     */
    for (i = 0; i < 5; i++) {
	if (!external_port[i].port) continue;
	FD_SET(external_port[i].fd, &readmask);
    }
    /*
     * set user fds in readmask.
     */
    for (i = 0; i < max_users; i++) {
	if (!all_users[i] || (all_users[i]->iflags & (CLOSING | CMD_IN_BUF)))
	    continue;
	/*
	 * if this user needs more input to make a complete command, set his
	 * fd so we can get it.
	 */
	FD_SET(all_users[i]->fd, &readmask);
	if (all_users[i]->message_length != 0)
	    FD_SET(all_users[i]->fd, &writemask);
    }
    /*
     * if addr_server_fd is set, set its fd in readmask.
     */
    if (addr_server_fd >= 0) {
	FD_SET(addr_server_fd, &readmask);
    }
#ifdef PACKAGE_SOCKETS
    /*
     * set fd's for efun sockets.
     */
    for (i = 0; i < max_lpc_socks; i++) {
	if (lpc_socks[i].state != CLOSED) {
	    if (lpc_socks[i].state != FLUSHING &&
		(lpc_socks[i].flags & S_WACCEPT) == 0)
		FD_SET(lpc_socks[i].fd, &readmask);
	    if (lpc_socks[i].flags & S_BLOCKED)
		FD_SET(lpc_socks[i].fd, &writemask);
	}
    }
#endif
}				/* make_selectmasks() */

/*
 * Process I/O.
 */
INLINE void process_io()
{
    int i;

    debug(256, ("@"));
    /*
     * check for new user connection.
     */
    for (i = 0; i < 5; i++) {
	if (!external_port[i].port) continue;
	if (FD_ISSET(external_port[i].fd, &readmask)) {
	    debug(512, ("process_io: NEW_USER\n"));
	    new_user_handler(i);
	}
    }
    /*
     * check for data pending on user connections.
     */
    for (i = 0; i < max_users; i++) {
	if (!all_users[i] || (all_users[i]->iflags & (CLOSING | CMD_IN_BUF)))
	    continue;
	if (all_users[i]->iflags & NET_DEAD) {
	    remove_interactive(all_users[i]->ob, 0);
	    continue;
	}
	if (FD_ISSET(all_users[i]->fd, &readmask)) {
	    debug(512, ("process_io: USER %d\n", i));
	    get_user_data(all_users[i]);
	    if (!all_users[i])
		continue;
	}
	if (FD_ISSET(all_users[i]->fd, &writemask))
	    flush_message(all_users[i]);
    }
#ifdef PACKAGE_SOCKETS
    /*
     * check for data pending on efun socket connections.
     */
    for (i = 0; i < max_lpc_socks; i++) {
	if (lpc_socks[i].state != CLOSED)
	    if (FD_ISSET(lpc_socks[i].fd, &readmask))
		socket_read_select_handler(i);
	if (lpc_socks[i].state != CLOSED)
	    if (FD_ISSET(lpc_socks[i].fd, &writemask))
		socket_write_select_handler(i);
    }
#endif
    /*
     * check for data pending from address server.
     */
    if (addr_server_fd >= 0) {
	if (FD_ISSET(addr_server_fd, &readmask)) {
	    debug(512, ("process_io: IP_DAEMON\n"));
	    hname_handler();
	}
    }
}

/*
 * This is the new user connection handler. This function is called by the
 * event handler when data is pending on the listening socket (new_user_fd).
 * If space is available, an interactive data structure is initialized and
 * the user is connected.
 */
static void new_user_handler P1(int, which)
{
    int new_socket_fd;
    struct sockaddr_in addr;
    int length;
    int i;
    object_t *ob;
    svalue_t *ret;

    length = sizeof(addr);
    debug(512, ("new_user_handler: accept on fd %d\n", external_port[which].fd));
    new_socket_fd = accept(external_port[which].fd,
			   (struct sockaddr *) & addr, (int *) &length);
    if (new_socket_fd < 0) {
#ifdef WINSOCK
	if (errno == WSAEWOULDBLOCK) {
#else
	if (errno == EWOULDBLOCK) {
#endif
	    debug(512, ("new_user_handler: accept: Operation would block\n"));
	} else {
	    debug_perror("new_user_handler: accept", 0);
	}
	return;
    }
#ifdef linux
    /*
     * according to Amylaar, 'accepted' sockets in Linux 0.99p6 don't
     * properly inherit the nonblocking property from the listening socket.
     */
    if (set_socket_nonblocking(new_socket_fd, 1) == -1) {
	debug_perror("new_user_handler: set_socket_nonblocking 1", 0);
	exit(8);
    }
#endif				/* linux */
    for (i = 0; i < max_users; i++)
	if (!all_users[i]) break;

    if (i == max_users) {
	if (all_users) {
	    all_users = RESIZE(all_users, max_users + 10, interactive_t *,
			       TAG_USERS, "new_user_handler");
	} else {
	    all_users = CALLOCATE(10, interactive_t *,
				  TAG_USERS, "new_user_handler");
	}
	while (max_users < i + 10)
	    all_users[max_users++] = 0;
    }

    command_giver = master_ob;
    master_ob->interactive =
	(interactive_t *)
	    DXALLOC(sizeof(interactive_t), TAG_INTERACTIVE,
		    "new_user_handler");
    total_users++;
#ifndef NO_ADD_ACTION
    master_ob->interactive->default_err_message.s = 0;
#endif
    master_ob->interactive->connection_type = external_port[which].kind;
    master_ob->flags |= O_ONCE_INTERACTIVE;
    /*
     * initialize new user interactive data structure.
     */
    master_ob->interactive->ob = master_ob;
    master_ob->interactive->input_to = 0;
    master_ob->interactive->iflags = 0;
    master_ob->interactive->text[0] = '\0';
    master_ob->interactive->text_end = 0;
    master_ob->interactive->text_start = 0;
    master_ob->interactive->carryover = NULL;
    master_ob->interactive->snoop_on = 0;
    master_ob->interactive->snoop_by = 0;
    master_ob->interactive->last_time = current_time;
#ifdef TRACE
    master_ob->interactive->trace_level = 0;
    master_ob->interactive->trace_prefix = 0;
#endif
#ifdef OLD_ED
    master_ob->interactive->ed_buffer = 0;
#endif
    master_ob->interactive->message_producer = 0;
    master_ob->interactive->message_consumer = 0;
    master_ob->interactive->message_length = 0;
    master_ob->interactive->num_carry = 0;
    master_ob->interactive->state = TS_DATA;
    master_ob->interactive->out_of_band = 0;
    all_users[i] = master_ob->interactive;
    all_users[i]->fd = new_socket_fd;
#ifdef F_QUERY_IP_PORT
    all_users[i]->local_port = external_port[which].port;
#endif
    set_prompt("> ");
    
    memcpy((char *) &all_users[i]->addr, (char *) &addr, length);
    debug(512, ("New connection from %s.\n", inet_ntoa(addr.sin_addr)));
    num_user++;
    /*
     * The user object has one extra reference. It is asserted that the
     * master_ob is loaded.
     */
    add_ref(master_ob, "new_user");
    push_number(external_port[which].port);
    ret = apply_master_ob(APPLY_CONNECT, 1);
    /* master_ob->interactive can be zero if the master object self
       destructed in the above (don't ask) */
    if (ret == 0 || ret == (svalue_t *)-1 || ret->type != T_OBJECT
	|| !master_ob->interactive) {
	if (master_ob->interactive)
	    remove_interactive(master_ob, 0);
	debug_message("Connection from %s aborted.\n", inet_ntoa(addr.sin_addr));
	return;
    }
    /*
     * There was an object returned from connect(). Use this as the user
     * object.
     */
    ob = ret->u.ob;
    if (ob->flags & O_HIDDEN)
	num_hidden++;
    ob->interactive = master_ob->interactive;
    ob->interactive->ob = ob;
    ob->flags |= O_ONCE_INTERACTIVE;
    /*
     * assume the existance of write_prompt and process_input in user.c
     * until proven wrong (after trying to call them).
     */
    ob->interactive->iflags |= (HAS_WRITE_PROMPT | HAS_PROCESS_INPUT);
    
    master_ob->flags &= ~O_ONCE_INTERACTIVE;
    master_ob->interactive = 0;
    free_object(master_ob, "reconnect");
    add_ref(ob, "new_user");
    command_giver = ob;
    if (addr_server_fd >= 0) {
	query_addr_name(ob);
    }
    
    if (external_port[which].kind == PORT_TELNET) {
	/* Ask permission to ask them for their terminal type */
	add_message(ob, telnet_do_ttype);
	/* Ask them for their window size */
	add_message(ob, telnet_do_naws);
    }
    
    logon(ob);
    debug(512, ("new_user_handler: end\n"));
    command_giver = 0;
}				/* new_user_handler() */

/*
 * This is the user command handler. This function is called when
 * a user command needs to be processed.
 * This function calls get_user_command() to get a user command.
 * One user command is processed per execution of this function.
 */
int process_user_command()
{
    char *user_command;
    static char buf[MAX_TEXT], *tbuf;
    object_t *save_current_object = current_object;
    object_t *save_command_giver = command_giver;
    interactive_t *ip;
    svalue_t *ret;

    buf[MAX_TEXT - 1] = '\0';
    /* WARNING: get_user_command() sets command_giver */
    if ((user_command = get_user_command())) {
#if defined(NO_ANSI) && defined(STRIP_BEFORE_PROCESS_INPUT)
	char *p;
	for (p = user_command; *p; p++) {
	    if (*p == 27) {
		char *q = buf;
		for (p = user_command; *p && p - user_command < MAX_TEXT-1; p++)
		    *q++ = ((*p == 27) ? ' ' : *p);
		*q = 0;
		user_command = buf;
		break;
	    }
	}
#endif
	
	if (command_giver->flags & O_DESTRUCTED) {
	    command_giver = save_command_giver;
	    current_object = save_current_object;
	    return 1;
	}
	ip = command_giver->interactive;
	if (!ip) return 1;
	current_interactive = command_giver;
	current_object = 0;
#ifndef NO_ADD_ACTION
	clear_notify(ip);
#endif
	update_load_av();
	debug(512, ("process_user_command: command_giver = /%s\n",
		    command_giver->name));
	tbuf = user_command;
	if ((user_command[0] == '!') && (
#ifdef OLD_ED
	      ip->ed_buffer ||
#endif
	      (ip->input_to && !(ip->iflags & NOESC)))) {
            if (ip->iflags & SINGLE_CHAR) {
                /* only 1 char ... switch to line buffer mode */
                ip->iflags |= WAS_SINGLE_CHAR;
                ip->iflags &= ~SINGLE_CHAR;
                add_message(ip->ob, telnet_no_single);
                /* come back later */
            } else {
                if (ip->iflags & WAS_SINGLE_CHAR) {
                    /* we now have a string ... switch back to char mode */
                    ip->iflags &= ~WAS_SINGLE_CHAR;
                    ip->iflags |= SINGLE_CHAR;
                    add_message(command_giver, telnet_yes_single);
		    VALIDATE_IP(ip, command_giver);
                }
		
	        if (ip->iflags & HAS_PROCESS_INPUT) {
		    copy_and_push_string(user_command + 1);
		    ret = apply(APPLY_PROCESS_INPUT, command_giver, 1, ORIGIN_DRIVER);
		    VALIDATE_IP(ip, command_giver);
		    if (!ret)
		        ip->iflags &= ~HAS_PROCESS_INPUT;
#ifndef NO_ADD_ACTION
		    if (ret && ret->type == T_STRING) {
		        strncpy(buf, ret->u.string, MAX_TEXT - 1);
			parse_command(buf, command_giver);
		    } else if (!ret || ret->type != T_NUMBER || !ret->u.number) {
			parse_command(tbuf+1, command_giver);
		    }
#endif
	        }
#ifndef NO_ADD_ACTION
	        else parse_command(tbuf + 1, command_giver);
#endif
            }
#ifdef OLD_ED
	} else if (ip->ed_buffer) {
	    ed_cmd(user_command);
#endif				/* ED */
	} else if (call_function_interactive(ip, user_command)) {
	    ;			/* do nothing */
	} else {
	    /*
	     * send a copy of user input back to user object to provide
	     * support for things like command history and mud shell
	     * programming languages.
	     */
	    if (ip->iflags & HAS_PROCESS_INPUT) {
		copy_and_push_string(user_command);
		ret = apply(APPLY_PROCESS_INPUT, command_giver, 1, ORIGIN_DRIVER);
		VALIDATE_IP(ip, command_giver);
		if (!ret)
		    ip->iflags &= ~HAS_PROCESS_INPUT;
#ifndef NO_ADD_ACTION
		if (ret && ret->type == T_STRING) {
		    strncpy(buf, ret->u.string, MAX_TEXT - 1);
		    parse_command(buf, command_giver);
		} else if (!ret || ret->type != T_NUMBER || !ret->u.number) {
		    parse_command(tbuf, command_giver);
		}
#endif
	    }
#ifndef NO_ADD_ACTION
	    else parse_command(tbuf, command_giver);
#endif
	}
	VALIDATE_IP(ip, command_giver);
	/*
	 * Print a prompt if user is still here.
	 */
	print_prompt(ip);
    failure:
	current_object = save_current_object;
	command_giver = save_command_giver;
	current_interactive = 0;
	return (1);
    }
    /* no more commands */
    current_object = save_current_object;
    command_giver = save_command_giver;
    current_interactive = 0;
    return 0;
}				/* process_user_command() */

#define HNAME_BUF_SIZE 200
/*
 * This is the hname input data handler. This function is called by the
 * master handler when data is pending on the hname socket (addr_server_fd).
 */

static void hname_handler()
{
    static char hname_buf[HNAME_BUF_SIZE];
    int num_bytes;
    int tmp;
    char *pp, *q;
    long laddr;

    if (addr_server_fd < 0) {
	return;
    }
    num_bytes = OS_socket_read(addr_server_fd, hname_buf, HNAME_BUF_SIZE);
    switch (num_bytes) {
    case -1:
	switch (errno) {
#ifdef EWOULDBLOCK
	case EWOULDBLOCK:
	    debug(512, ("hname_handler: read on fd %d: Operation would block.\n",
			addr_server_fd));
	    break;
#endif
	default:
	    debug_message("hname_handler: read on fd %d\n", addr_server_fd);
	    debug_perror("hname_handler: read", 0);
	    tmp = addr_server_fd;
	    addr_server_fd = -1;
	    OS_socket_close(tmp);
	    return;
	}
	break;
    case 0:
	debug_message("hname_handler: closing address server connection.\n");
	tmp = addr_server_fd;
	addr_server_fd = -1;
	OS_socket_close(tmp);
	return;
    default:
	hname_buf[num_bytes] = '\0';
	debug(512, ("hname_handler: address server replies: %s", hname_buf));
	if (hname_buf[0] >= '0' && hname_buf[0] <= '9') {
	    laddr = inet_addr(hname_buf);
	    if (laddr != -1) {
		pp = strchr(hname_buf, ' ');
		if (pp) {
		    *pp = 0;
		    pp++;
		    q = strchr(pp, '\n');
		    if (q) {
			*q = 0;
			if (strcmp(pp, "0"))
			    add_ip_entry(laddr, pp);
			got_addr_number(pp, hname_buf);	/* Recognises this as
							 * failure. */
		    }
		}
	    }
	} else {
	    char *r;

/* This means it was a name lookup... */
	    pp = strchr(hname_buf, ' ');
	    if (pp) {
		*pp = 0;
		pp++;
		r = strchr(pp, '\n');
		if (r)
		    *r = 0;
		got_addr_number(pp, hname_buf);
	    }
	}
	break;
    }
}				/* hname_handler() */

/*
 * Read pending data for a user into user->interactive->text.
 * This also does telnet negotiation.
 */
static void get_user_data P1(interactive_t *, ip)
{
    static char buf[MAX_TEXT];
    int text_space;
    int num_bytes;
    
    /*
     * this /3 is here because of the trick copy_chars() uses to allow empty
     * commands. it needs to be fixed right. later.
     */
    if (ip->connection_type == PORT_TELNET) {
	text_space = (MAX_TEXT - ip->text_end - 1) / 3;
    } else {
	text_space = sizeof(buf) - 1;
    }
    /*
     * Check if we need more space.
     */
    if (text_space < MAX_TEXT/16) {
	int l = ip->text_end - ip->text_start;
	
	memmove(ip->text, ip->text + ip->text_start, l + 1);
	ip->text_start = 0;
	ip->text_end = l;
	text_space = (MAX_TEXT - ip->text_end - 1) / 3;
	if (text_space < MAX_TEXT/16) {
	    /* almost 2k data without a newline.  Flush it, otherwise
	       text_space will eventually go to zero and dest the user. */
	    ip->text_start = 0;
	    ip->text_end = 0;
	    text_space = MAX_TEXT / 3;
	}
    }
    /*
     * read user data.
     */
    debug(512, ("get_user_data: read on fd %d\n", ip->fd));
    num_bytes = OS_socket_read(ip->fd, buf, text_space);
    switch (num_bytes) {
    case 0:
	if (ip->iflags & CLOSING)
	    debug_message("get_user_data: tried to read from closing fd.\n");
	remove_interactive(ip->ob, 0);
	return;
    case -1:
#ifdef EWOULDBLOCK
	if (errno == EWOULDBLOCK) {
	    debug(512, ("get_user_data: read on fd %d: Operation would block.\n",
			ip->fd));
	} else
#endif
#ifdef WSAEWOULDBLOCK
	if (errno == WSAEWOULDBLOCK) {
	    debug(512, ("get_user_data: read on fd %d: Operation would block.\n",
			ip->fd));
	} else
#endif
	{
	    debug_message("get_user_data: read on fd %d\n", ip->fd);
	    debug_perror("get_user_data: read", 0);
	    remove_interactive(ip->ob, 0);
	    return;
	}
	break;
    default:
	buf[num_bytes] = '\0';
	switch (ip->connection_type) {
	case PORT_TELNET:
	    /*
	     * replace newlines with nulls and catenate to buffer. Also do all
	     * the useful telnet negotation at this point too. Rip out the sub
	     * option stuff and send back anything non useful we feel we have
	     * to.
	     */
	    ip->text_end += copy_chars((unsigned char *)buf, (unsigned char *)ip->text + ip->text_end, num_bytes, ip);
	    /*
	     * now, text->end is just after the last char read. If last char
	     was a nl, char *before* text_end will be null.
	     */
	    ip->text[ip->text_end] = '\0';
	    /*
	     * handle snooping - snooper does not see type-ahead. seems like
	     * that would be very inefficient, for little functional gain.
	     */
	    if (ip->snoop_by && !(ip->iflags & NOECHO))
		receive_snoop(buf, ip->snoop_by->ob);

	    /*
	     * set flag if new data completes command.
	     */
	    if (cmd_in_buf(ip))
		ip->iflags |= CMD_IN_BUF;
	    break;
	case PORT_ASCII:
	    {
		char *nl, *str;
		char *p = ip->text + ip->text_start;
		
		while ((nl = memchr(p, '\n', ip->text_end - ip->text_start))) {
		    ip->text_start = (nl + 1) - ip->text;

		    *nl = 0;
		    str = new_string(nl - p, "PORT_ASCII");
		    memcpy(str, p, nl - p + 1);
		    if (!(ip->ob->flags & O_DESTRUCTED)) {
			push_malloced_string(str);
			apply(APPLY_PROCESS_INPUT, ip->ob, 1, ORIGIN_DRIVER);
		    }
		    if (ip->text_start == ip->text_end) {
			ip->text_start = 0;
			ip->text_end = 0;
			break;
		    } else {
			p = nl + 1;
		    }
		}
		break;
	    }
	case PORT_BINARY:
	    {
		buffer_t *buffer;
		svalue_t *ret;
		
		buffer = allocate_buffer(num_bytes);
		memcpy(buffer->item, buf, num_bytes);
		
		push_refed_buffer(buffer);
		ret = apply(APPLY_PROCESS_INPUT, ip->ob, 1, ORIGIN_DRIVER);
		break;
	    }
	}
    }
}				/* get_user_data() */

/*
 * Return the first cmd of the next user in sequence that has a complete cmd
 * in their buffer.
 * CmdsGiven is used to allow users in ED to send more cmds (if they have
 * them queued up) than users not in ED.
 * This should also return a value if there is something in the
 * buffer and we are supposed to be in single character mode.
 */
#define IncCmdGiver     NextCmdGiver = (NextCmdGiver == 0 ? max_users - 1: \
                                        NextCmdGiver - 1)

static char *get_user_command()
{
    static int NextCmdGiver = 0;

    int i;
    interactive_t *ip;
    char *user_command = NULL;
    static char buf[MAX_TEXT];

    /*
     * find and return a user command.
     */
    for (i = 0; i < max_users; i++) {
	ip = all_users[NextCmdGiver];
	if (ip && ip->message_length) {
	    object_t *ob = ip->ob;
	    flush_message(ip);
	    if (!IP_VALID(ip, ob))
		ip = 0;
	}
	if (ip && ip->iflags & CMD_IN_BUF) {
	    user_command = first_cmd_in_buf(ip);
	    if (user_command) 
		break;
	    else
		ip->iflags &= ~CMD_IN_BUF;
	}
	if (NextCmdGiver-- == 0)
	    NextCmdGiver = max_users - 1;
    }
    /*
     * no cmds found; return 0.
     */
    if (!ip || !user_command)
	return 0;
    /*
     * we have a user cmd -- return it. If user has only one partially
     * completed cmd left after this, move it to the start of his buffer; new
     * stuff will be appended.
     */
    debug(512, ("get_user_command: user_command = (%s)\n", user_command));
    command_giver = ip->ob;
    /*
     * telnet option parsing and negotiation.
     */
    telnet_neg(buf, user_command);
    /*
     * move input buffer pointers to next command.
     */
    next_cmd_in_buf(ip);
    if (!cmd_in_buf(ip))
	ip->iflags &= ~CMD_IN_BUF;

    if (NextCmdGiver-- == 0)
	NextCmdGiver = max_users - 1;

    if (ip->iflags & NOECHO) {
	/*
	 * Must not enable echo before the user input is received.
	 */
	add_message(command_giver, telnet_no_echo);
	ip->iflags &= ~NOECHO;
    }
    ip->last_time = current_time;
    return (buf);
}				/* get_user_command() */

/*
 * find the first character of the next complete cmd in a buffer, 0 if no
 * completed cmd.  There is a completed cmd if there is a null between
 * text_start and text_end.  Zero length commands are discarded (as occur
 * between <cr> and <lf>).  Update text_start if we have to skip leading
 * nulls.
 * This should return true when in single char mode and there is
 * Anything at all in the buffer.
 */
static char *first_cmd_in_buf P1(interactive_t *, ip)
{
    char *p, *q;

    p = ip->text + ip->text_start;

    /*
     * skip null input.
     */
    while ((p < (ip->text + ip->text_end)) && !*p)
	p++;

    ip->text_start = p - ip->text;

    if (ip->text_start >= ip->text_end) {
	ip->text_start = ip->text_end = 0;
	ip->text[0] = '\0';
	return 0;
    }
    /* If we got here, must have something in the array */
    if (ip->iflags & SINGLE_CHAR) {
	/* We need to return true here... */
	return (ip->text + ip->text_start);
    }
    /*
     * find end of cmd.
     */
    while ((p < (ip->text + ip->text_end)) && *p)
	p++;
    /*
     * null terminated; was command.
     */
    if (p < ip->text + ip->text_end)
	return (ip->text + ip->text_start);
    /*
     * have a partial command at end of buffer; move it to start, return
     * null. if it can't move down, truncate it and return it as cmd.
     */
    p = ip->text + ip->text_start;
    q = ip->text;
    while (p < (ip->text + ip->text_end))
	*(q++) = *(p++);

    ip->text_end -= ip->text_start;
    ip->text_start = 0;
    if (ip->text_end > MAX_TEXT - 2) {
	ip->text[ip->text_end - 2] = '\0';	/* nulls to truncate */
	ip->text[ip->text_end - 1] = '\0';	/* nulls to truncate */
	ip->text_end--;
	return (ip->text);
    }
    /*
     * buffer not full and no newline - no cmd.
     */
    return 0;
}				/* first_command_in_buf() */

/*
 * return(1) if there is a complete command in ip->text, otherwise return(0).
 */
static int cmd_in_buf P1(interactive_t *, ip)
{
    char *p;

    p = ip->text + ip->text_start;

    /*
     * skip null input.
     */
    while ((p < (ip->text + ip->text_end)) && !*p)
	p++;

    if ((p - ip->text) >= ip->text_end) {
	return (0);
    }
    /* If we get here, must have something in the buffer */
    if (ip->iflags & SINGLE_CHAR) {
	return (1);
    }
    /*
     * find end of cmd.
     */
    while ((p < (ip->text + ip->text_end)) && *p)
	p++;
    /*
     * null terminated; was command.
     */
    if (p < ip->text + ip->text_end)
	return (1);
    /*
     * no newline - no cmd.
     */
    return (0);
}				/* cmd_in_buf() */

/*
 * move pointers to next cmd, or clear buf.
 */
static void next_cmd_in_buf P1(interactive_t *, ip)
{
    char *p = ip->text + ip->text_start;

    while (*p && p < ip->text + ip->text_end)
	p++;
    /*
     * skip past any nulls at the end.
     */
    while (!*p && p < ip->text + ip->text_end)
	p++;
    if (p < ip->text + ip->text_end)
	ip->text_start = p - ip->text;
    else {
	ip->text_start = ip->text_end = 0;
	ip->text[0] = '\0';
    }
}				/* next_cmd_in_buf() */

/*
 * Remove an interactive user immediately.
 */
void remove_interactive P2(object_t *, ob, int, dested)
{
    int idx;
    /* don't have to worry about this dangling, since this is the routine
     * that causes this to dangle elsewhere, and we are protected from
     * getting called recursively by CLOSING.  safe_apply() should be
     * used here, since once we start this process we can't back out,
     * so jumping out with an error would be bad.
     */
    interactive_t *ip = ob->interactive;
    
    if (!ip) return;
    
    if (ip->iflags & CLOSING) {
	if (!dested)
	    debug_message("Double call to remove_interactive()\n");
	return;
    }

    debug(512, ("Closing connection from %s.\n",
 		inet_ntoa(ip->addr.sin_addr)));

    flush_message(ip);
    ip->iflags |= CLOSING;

#ifdef F_ED
    if (ip->ed_buffer) {
 	save_ed_buffer(ob);
    }
#endif

    if (!dested) {
        /*
	 * auto-notification of net death
	 */
	safe_apply(APPLY_NET_DEAD, ob, 0, ORIGIN_DRIVER);
    }
    
    if (ip->snoop_by) {
	ip->snoop_by->snoop_on = 0;
 	ip->snoop_by = 0;
    }
    if (ip->snoop_on) {
 	ip->snoop_on->snoop_by = 0;
 	ip->snoop_on = 0;
    }

    debug(512, ("remove_interactive: closing fd %d\n", ip->fd));
    if (OS_socket_close(ip->fd) == -1) {
 	debug_perror("remove_interactive: close", 0);
    }
    if (ob->flags & O_HIDDEN)
 	num_hidden--;
    num_user--;
#ifndef NO_ADD_ACTION
    clear_notify(ip);
#endif
    if (ip->input_to) {
	free_object(ip->input_to->ob, "remove_interactive");
	free_sentence(ip->input_to);
	if (ip->num_carry > 0)
	    free_some_svalues(ip->carryover, ip->num_carry);
	ip->carryover = NULL;
	ip->num_carry = 0;
	ip->input_to = 0;
    }
    for (idx = 0; idx < max_users; idx++)
	if (all_users[idx] == ip) break;
    DEBUG_CHECK(idx == max_users, "remove_interactive: could not find and remove user!\n");
    FREE(ip);
    total_users--;
    ob->interactive = 0;
    all_users[idx] = 0;
    free_object(ob, "remove_interactive");
    return;
}				/* remove_interactive() */

static int call_function_interactive P2(interactive_t *, i, char *, str)
{
    object_t *ob;
    funptr_t *funp;
    char *function;
    svalue_t *args;
    sentence_t *sent;
    int num_arg;

    i->iflags &= ~NOESC;
    if (!(sent = i->input_to))
	return (0);

    /*
     * Special feature: input_to() has been called to setup a call to a
     * function.
     */
    if (sent->ob->flags & O_DESTRUCTED) {
	/* Sorry, the object has selfdestructed ! */
	free_object(sent->ob, "call_function_interactive");
	free_sentence(sent);
	i->input_to = 0;
	if (i->num_carry)
	    free_some_svalues(i->carryover, i->num_carry);
	i->carryover = NULL;
	i->num_carry = 0;
	return (0);
    }
    /*
     * We must all references to input_to fields before the call to apply(),
     * because someone might want to set up a new input_to().
     */
    free_object(sent->ob, "call_function_interactive");
    /* we put the function on the stack in case of an error */
    sp++;
    if (sent->flags & V_FUNCTION) {
      function = 0;
      sp->type = T_FUNCTION;
      sp->u.fp = funp = sent->function.f;
      funp->hdr.ref++;
    } else {
      sp->type = T_STRING;
      sp->subtype = STRING_SHARED;
      sp->u.string = function = sent->function.s;
      ref_string(function);
    }
    ob = sent->ob;
    free_sentence(sent);

    /*
     * If we have args, we have to copy them, so the svalues on the
     * interactive struct can be FREEd
     */
    num_arg = i->num_carry;
    if (num_arg) {
	args = i->carryover;
	i->num_carry = 0;
	i->carryover = NULL;
    } else
	args = NULL;

    i->input_to = 0;
    if (i->iflags & SINGLE_CHAR) {
	/*
	 * clear single character mode
	 */
	i->iflags &= ~SINGLE_CHAR;
	add_message(i->ob, telnet_no_single);
    }

    copy_and_push_string(str);
    /*
     * If we have args, we have to push them onto the stack in the order they
     * were in when we got them.  They will be popped off by the called
     * function.
     */
    if (args) {
	transfer_push_some_svalues(args, num_arg);
	FREE(args);
    }
    /* current_object no longer set */
    if (function) {
	if (function[0] == APPLY___INIT_SPECIAL_CHAR)
	    error("Illegal function name.\n");
       (void) apply(function, ob, num_arg + 1, ORIGIN_DRIVER);
    } else
       call_function_pointer(funp, num_arg + 1);

    pop_stack();		/* remove `function' from stack */

    return (1);
}				/* call_function_interactive() */

int set_call P3(object_t *, ob, sentence_t *, sent, int, flags)
{
    if (ob == 0 || sent == 0)
	return (0);
    if (ob->interactive == 0 || ob->interactive->input_to)
	return (0);
    ob->interactive->input_to = sent;
    ob->interactive->iflags |= (flags & (I_NOECHO | I_NOESC | I_SINGLE_CHAR));
    if (flags & I_NOECHO)
	add_message(ob, telnet_yes_echo);
    if (flags & I_SINGLE_CHAR)
	add_message(ob, telnet_yes_single);
    return (1);
}				/* set_call() */

void set_prompt P1(char *, str)
{
    if (command_giver && command_giver->interactive) {
	command_giver->interactive->prompt = str;
    }
}				/* set_prompt() */

/*
 * Print the prompt, but only if input_to not is disabled.
 */
static void print_prompt P1(interactive_t*, ip)
{
    object_t *ob = ip->ob;
    
    if (ip->input_to == 0) {
	/* give user object a chance to write its own prompt */
	if (!(ip->iflags & HAS_WRITE_PROMPT))
	    tell_object(ip->ob, ip->prompt);
#ifdef OLD_ED
	else if (ip->ed_buffer)
	    tell_object(ip->ob, ip->prompt);
#endif
	else if (!apply(APPLY_WRITE_PROMPT, ip->ob, 0, ORIGIN_DRIVER)) {
	    if (!IP_VALID(ip, ob)) return;
	    ip->iflags &= ~HAS_WRITE_PROMPT;
	    tell_object(ip->ob, ip->prompt);
	}
    }
    if (!IP_VALID(ip, ob)) return;
    /*
     * Put the IAC GA thing in here... Moved from before writing the prompt;
     * vt src says it's a terminator. Should it be inside the no-input_to
     * case? We'll see, I guess.
     */
    if (ip->iflags & USING_TELNET)
	add_message(command_giver, telnet_ga);
    if (!IP_VALID(ip, ob)) return;
    flush_message(ip);
}				/* print_prompt() */

/*
 * Let object 'me' snoop object 'you'. If 'you' is 0, then turn off
 * snooping.
 *
 * This routine is almost identical to the old set_snoop. The main
 * difference is that the routine writes nothing to user directly,
 * all such communication is taken care of by the mudlib. It communicates
 * with master.c in order to find out if the operation is permissble or
 * not. The old routine let everyone snoop anyone. This routine also returns
 * 0 or 1 depending on success.
 */
int new_set_snoop P2(object_t *, me, object_t *, you)
{
    interactive_t *on, *by, *tmp;

    /*
     * Stop if people managed to quit before we got this far.
     */
    if (me->flags & O_DESTRUCTED)
	return (0);
    if (you && (you->flags & O_DESTRUCTED))
	return (0);
    /*
     * Find the snooper && snoopee.
     */
    if (!me->interactive)
	error("First argument of snoop() is not interactive!\n");

    by = me->interactive;
    
    if (you) {
	if (!you->interactive)
	    error("Second argument of snoop() is not interactive!\n");
	on = you->interactive;
    } else {
	/*
	 * Stop snoop.
	 */
	if (by->snoop_on) {
	    by->snoop_on->snoop_by = 0;
	    by->snoop_on = 0;
	}
	return 1;
    }

    /*
     * Protect against snooping loops.
     */
    for (tmp = on; tmp; tmp = tmp->snoop_on) {
	if (tmp == by)
	    return (0);
    }
    
    /*
     * Terminate previous snoop, if any.
     */
    if (by->snoop_on) {
	by->snoop_on->snoop_by = 0;
	by->snoop_on = 0;
    }
    if (on->snoop_by) {
	on->snoop_by->snoop_on = 0;
	on->snoop_by = 0;
    }
    on->snoop_by = by;
    by->snoop_on = on;
    return (1);
}				/* set_new_snoop() */

/*
 * Bit of a misnomer now.  But I can't be bothered changeing the
 * name.  This will handle backspace resolution amongst other things,
 * (Pinkfish change)
 */
static void telnet_neg P2(char *, to, char *, from)
{
    int ch;
    char *first = to;

    while (1) {
	ch = *from++;
	switch (ch) {
	case '\b':		/* Backspace */
	case 0x7f:		/* Delete */
	    if (to <= first)
		continue;
	    to -= 1;
	    continue;
	default:
	    *to++ = ch;
	    if (ch == 0)
		return;
	}			/* switch() */
    }				/* while() */
}				/* telnet_neg() */

static void query_addr_name P1(object_t *, ob)
{
    static char buf[100];
    static char *dbuf = &buf[sizeof(int) + sizeof(int) + sizeof(int)];
    int msglen;
    int msgtype;

    sprintf(dbuf, "%s", query_ip_number(ob));
    msglen = sizeof(int) + strlen(dbuf) +1;

    msgtype = DATALEN;
    memcpy(buf, (char *) &msgtype, sizeof(msgtype));
    memcpy(&buf[sizeof(int)], (char *) &msglen, sizeof(msglen));

    msgtype = NAMEBYIP;
    memcpy(&buf[sizeof(int) + sizeof(int)], (char *) &msgtype, sizeof(msgtype));
    debug(512, ("query_addr_name: sent address server %s\n", dbuf));

    if (OS_socket_write(addr_server_fd, buf, msglen + sizeof(int) + sizeof(int)) == -1) {
	switch (errno) {
	case EBADF:
	    debug_message("Address server has closed connection.\n");
	    addr_server_fd = -1;
	    break;
	default:
	    debug_perror("query_addr_name: write", 0);
	    break;
	}
    }
}				/* query_addr_name() */

#define IPSIZE 200
typedef struct {
    char *name, *call_back;
    object_t *ob_to_call;
} ipnumberentry_t;

static ipnumberentry_t ipnumbertable[IPSIZE];

/*
 * Does a call back on the current_object with the function call_back.
 */
int query_addr_number P2(char *, name, char *, call_back)
{
    static char buf[100];
    static char *dbuf = &buf[sizeof(int) + sizeof(int) + sizeof(int)];
    int msglen;
    int msgtype;

    if ((addr_server_fd < 0) || (strlen(name) >=
		  100 - (sizeof(msgtype) + sizeof(msglen) + sizeof(int)))) {
	share_and_push_string(name);
	push_undefined();
	apply(call_back, current_object, 2, ORIGIN_DRIVER);
	return 0;
    }
    strcpy(dbuf, name);
    msglen = sizeof(int) + strlen(name) +1;

    msgtype = DATALEN;
    memcpy(buf, (char *) &msgtype, sizeof(msgtype));
    memcpy(&buf[sizeof(int)], (char *) &msglen, sizeof(msglen));

    msgtype = (name[0] >= '0' && name[0] <= '9') ? NAMEBYIP : IPBYNAME;
    memcpy(&buf[sizeof(int) + sizeof(int)], (char *) &msgtype, sizeof(msgtype));

    debug(512, ("query_addr_number: sent address server %s\n", dbuf));

    if (OS_socket_write(addr_server_fd, buf, msglen + sizeof(int) + sizeof(int)) == -1) {
	switch (errno) {
	case EBADF:
	    debug_message("Address server has closed connection.\n");
	    addr_server_fd = -1;
	    break;
	default:
	    debug_perror("query_addr_name: write", 0);
	    break;
	}
	share_and_push_string(name);
	push_undefined();
	apply(call_back, current_object, 2, ORIGIN_DRIVER);
	return 0;
    } else {
	int i;

/* We put ourselves into the pending name lookup entry table */
/* Find the first free entry */
	for (i = 0; i < IPSIZE && ipnumbertable[i].name; i++)
	    ;
	if (i == IPSIZE) {
/* We need to error...  */
	    share_and_push_string(name);
	    push_undefined();
	    apply(call_back, current_object, 2, ORIGIN_DRIVER);
	    return 0;
	}
/* Create our entry... */
	ipnumbertable[i].name = make_shared_string(name);
	ipnumbertable[i].call_back = make_shared_string(call_back);
	ipnumbertable[i].ob_to_call = current_object;
	add_ref(current_object, "query_addr_number: ");
	return i + 1;
    }
}				/* query_addr_number() */

static void got_addr_number P2(char *, number, char *, name)
{
    int i;
    char *theName, *theNumber;

    /* First remove all the dested ones... */
    for (i = 0; i < IPSIZE; i++)
	if (ipnumbertable[i].name
	    && ipnumbertable[i].ob_to_call->flags & O_DESTRUCTED) {
	    free_string(ipnumbertable[i].call_back);
	    free_string(ipnumbertable[i].name);
	    free_object(ipnumbertable[i].ob_to_call, "got_addr_number: ");
	    ipnumbertable[i].name = NULL;
	}
    for (i = 0; i < IPSIZE; i++) {
	if (ipnumbertable[i].name && strcmp(name, ipnumbertable[i].name)== 0) {
	    /* Found one, do the call back... */
	    theName = ipnumbertable[i].name;
	    theNumber = number;
	    
	    if (isdigit(theName[0])) {
		char *tmp;
		
		tmp = theName;
		theName = theNumber;
		theNumber = tmp;
	    }
	    if (strcmp(theName, "0")) {
		share_and_push_string(theName);
	    } else {
		push_undefined();
	    }
	    if (strcmp(number, "0")) {
		share_and_push_string(theNumber);
	    } else {
		push_undefined();
	    }
	    push_number(i + 1);
	    safe_apply(ipnumbertable[i].call_back, ipnumbertable[i].ob_to_call,
		       3, ORIGIN_DRIVER);
	    free_string(ipnumbertable[i].call_back);
	    free_string(ipnumbertable[i].name);
	    free_object(ipnumbertable[i].ob_to_call, "got_addr_number: ");
	    ipnumbertable[i].name = NULL;
	}
    }
}				/* got_addr_number() */

#undef IPSIZE
#define IPSIZE 200
typedef struct {
    long addr;
    char *name;
} ipentry_t;

static ipentry_t iptable[IPSIZE];
static int ipcur;

#ifdef DEBUGMALLOC_EXTENSIONS
void mark_iptable() {
    int i;

    for (i=0; i < IPSIZE; i++)
	if (iptable[i].name)
	    EXTRA_REF(BLOCK(iptable[i].name))++;
}
#endif

char *query_ip_name P1(object_t *, ob)
{
    int i;

    if (ob == 0)
	ob = command_giver;
    if (!ob || ob->interactive == 0)
	return NULL;
    for (i = 0; i < IPSIZE; i++) {
	if (iptable[i].addr == ob->interactive->addr.sin_addr.s_addr &&
	    iptable[i].name)
	    return (iptable[i].name);
    }
    return (inet_ntoa(ob->interactive->addr.sin_addr));
}

static void add_ip_entry P2(long, addr, char *, name)
{
    int i;

    for (i = 0; i < IPSIZE; i++) {
	if (iptable[i].addr == addr)
	    return;
    }
    iptable[ipcur].addr = addr;
    if (iptable[ipcur].name)
	free_string(iptable[ipcur].name);
    iptable[ipcur].name = make_shared_string(name);
    ipcur = (ipcur + 1) % IPSIZE;
}

char *query_ip_number P1(object_t *, ob)
{
    if (ob == 0)
	ob = command_giver;
    if (!ob || ob->interactive == 0)
	return 0;
    return (inet_ntoa(ob->interactive->addr.sin_addr));
}

#ifndef INET_NTOA_OK
/*
 * Note: if the address string is "a.b.c.d" the address number is
 *       a * 256^3 + b * 256^2 + c * 256 + d
 */
char *inet_ntoa P1(struct in_addr, ad)
{
    u_long s_ad;
    int a, b, c, d;
    static char addr[20];	/* 16 + 1 should be enough */

    s_ad = ad.s_addr;
    d = s_ad % 256;
    s_ad /= 256;
    c = s_ad % 256;
    s_ad /= 256;
    b = s_ad % 256;
    a = s_ad / 256;
    sprintf(addr, "%d.%d.%d.%d", a, b, c, d);
    return (addr);
}
#endif				/* INET_NTOA_OK */

char *query_host_name()
{
    static char name[40];

    gethostname(name, sizeof(name));
    name[sizeof(name) - 1] = '\0';	/* Just to make sure */
    return (name);
}				/* query_host_name() */

object_t *query_snoop P1(object_t *, ob)
{
    if (!ob->interactive || (ob->interactive->snoop_by == 0))
	return (0);
    return (ob->interactive->snoop_by->ob);
}				/* query_snoop() */

object_t *query_snooping P1(object_t *, ob)
{
    if (!ob->interactive || (ob->interactive->snoop_on == 0))
	return (0);
    return (ob->interactive->snoop_on->ob);
}				/* query_snooping() */

int query_idle P1(object_t *, ob)
{
    if (!ob->interactive)
	error("query_idle() of non-interactive object.\n");
    return (current_time - ob->interactive->last_time);
}				/* query_idle() */

#ifndef NO_ADD_ACTION
void notify_no_command()
{
    union string_or_func p;
    svalue_t *v;

    if (!command_giver || !command_giver->interactive)
	return;
    p = command_giver->interactive->default_err_message;
    if (command_giver->interactive->iflags & NOTIFY_FAIL_FUNC) {
	v = call_function_pointer(p.f, 0);
	free_funp(p.f);
	if (v && v->type == T_STRING) {
	    tell_object(command_giver, v->u.string);
	}
	command_giver->interactive->iflags &= ~NOTIFY_FAIL_FUNC;
	command_giver->interactive->default_err_message.s = 0;
    } else {
	if (p.s) {
	    tell_object(command_giver, p.s);
	    free_string(p.s);
	    command_giver->interactive->default_err_message.s = 0;
	} else {
	    tell_object(command_giver, default_fail_message);
	}
    }
}				/* notify_no_command() */

static void clear_notify P1(interactive_t *, ip)
{
    union string_or_func dem;

    dem = ip->default_err_message;
    if (ip->iflags & NOTIFY_FAIL_FUNC) {
	free_funp(dem.f);
	ip->iflags &= ~NOTIFY_FAIL_FUNC;
    }
    else if (dem.s)
	free_string(dem.s);
    ip->default_err_message.s = 0;
}				/* clear_notify() */

void set_notify_fail_message P1(char *, str)
{
    if (!command_giver || !command_giver->interactive)
	return;
    clear_notify(command_giver->interactive);
    command_giver->interactive->default_err_message.s = make_shared_string(str);
}				/* set_notify_fail_message() */

void set_notify_fail_function P1(funptr_t *, fp)
{
    if (!command_giver || !command_giver->interactive)
	return;
    clear_notify(command_giver->interactive);
    command_giver->interactive->iflags |= NOTIFY_FAIL_FUNC;
    command_giver->interactive->default_err_message.f = fp;
    fp->hdr.ref++;
}				/* set_notify_fail_message() */
#endif /* NO_ADD_ACTION */

int replace_interactive P2(object_t *, ob, object_t *, obfrom)
{
    if (ob->interactive) {
	error("Bad argument 1 to exec()\n");
    }
    if (!obfrom->interactive) {
	error("Bad argument 2 to exec()\n");
    }
    if ((ob->flags & O_HIDDEN) != (obfrom->flags & O_HIDDEN)) {
	if (ob->flags & O_HIDDEN) {
	    num_hidden++;
	} else {
	    num_hidden--;
	}
    }
    ob->interactive = obfrom->interactive;
    /*
     * assume the existance of write_prompt and process_input in user.c until
     * proven wrong (after trying to call them).
     */
    ob->interactive->iflags |= (HAS_WRITE_PROMPT | HAS_PROCESS_INPUT);
    obfrom->interactive = 0;
    ob->interactive->ob = ob;
    ob->flags |= O_ONCE_INTERACTIVE;
    obfrom->flags &= ~O_ONCE_INTERACTIVE;
    add_ref(ob, "exec");
    free_object(obfrom, "exec");
    if (obfrom == command_giver) {
	command_giver = ob;
    }
    return (1);
}				/* replace_interactive() */

void outbuf_zero P1(outbuffer_t *, outbuf) {
    outbuf->real_size = 0;
    outbuf->buffer = 0;
}

int outbuf_extend P2(outbuffer_t *, outbuf, int, l)
{
    int limit;

    DEBUG_CHECK(l < 0, "Negative length passed to outbuf_extend.\n");
    
    if (outbuf->buffer) {
	limit = MSTR_SIZE(outbuf->buffer);
	if (outbuf->real_size + l > limit) {
	    if (outbuf->real_size == USHRT_MAX) return 0; /* TRUNCATED */

	    /* assume it's going to grow some more */
	    limit = (outbuf->real_size + l) * 2;
	    if (limit > USHRT_MAX) {
		limit = outbuf->real_size + l;
		if (limit > USHRT_MAX) {
		    outbuf->buffer = extend_string(outbuf->buffer, USHRT_MAX);
		    return USHRT_MAX - outbuf->real_size;
		}
	    }
	    outbuf->buffer = extend_string(outbuf->buffer, limit);
	}
    } else {
	outbuf->buffer = new_string(l, "outbuf_add");
	outbuf->real_size = 0;
    }
    return l;
}

void outbuf_add P2(outbuffer_t *, outbuf, char *, str)
{
    int l, limit;
    
    if (!outbuf) return;
    l = strlen(str);
    if (outbuf->buffer) {
	limit = MSTR_SIZE(outbuf->buffer);
	if (outbuf->real_size + l > limit) {
	    if (outbuf->real_size == USHRT_MAX) return; /* TRUNCATED */

	    /* assume it's going to grow some more */
	    limit = (outbuf->real_size + l) * 2;
	    if (limit > USHRT_MAX) {
		limit = outbuf->real_size + l;
		if (limit > USHRT_MAX) {
		    outbuf->buffer = extend_string(outbuf->buffer, USHRT_MAX);
		    strncpy(outbuf->buffer + outbuf->real_size, str,
			    USHRT_MAX - outbuf->real_size);
		    outbuf->buffer[USHRT_MAX] = 0;
		    outbuf->real_size = USHRT_MAX;
		    return;
		}
	    }
	    outbuf->buffer = extend_string(outbuf->buffer, limit);
	}
    } else {
	outbuf->buffer = new_string(l, "outbuf_add");
	outbuf->real_size = 0;
    }
    strcpy(outbuf->buffer + outbuf->real_size, str);
    outbuf->real_size += l;
}

void outbuf_addchar P2(outbuffer_t *, outbuf, char, c)
{
    int limit;
    
    if (!outbuf) return;

    if (outbuf->buffer) {
	limit = MSTR_SIZE(outbuf->buffer);
	if (outbuf->real_size + 1 > limit) {
	    if (outbuf->real_size == USHRT_MAX) return; /* TRUNCATED */

	    /* assume it's going to grow some more */
	    limit = (outbuf->real_size + 1) * 2;
	    if (limit > USHRT_MAX) {
		limit = outbuf->real_size + 1;
		if (limit > USHRT_MAX) {
		    outbuf->buffer = extend_string(outbuf->buffer, USHRT_MAX);
		    *(outbuf->buffer + outbuf->real_size) = c;
		    outbuf->buffer[USHRT_MAX] = 0;
		    outbuf->real_size = USHRT_MAX;
		    return;
		}
	    }
	    outbuf->buffer = extend_string(outbuf->buffer, limit);
	}
    } else {
	outbuf->buffer = new_string(80, "outbuf_add");
	outbuf->real_size = 0;
    }
    *(outbuf->buffer + outbuf->real_size++) = c;
    *(outbuf->buffer + outbuf->real_size) = 0;
}

void outbuf_addv P2V(outbuffer_t *, outbuf, char *, format)
{
    char buf[LARGEST_PRINTABLE_STRING + 1];
    va_list args;
    V_DCL(char *format);
    V_DCL(outbuffer_t *outbuf);

    V_START(args, format);
    V_VAR(outbuffer_t *, outbuf, args);
    V_VAR(char *, format, args);

    vsprintf(buf, format, args);
    va_end(args);

    if (!outbuf) return;
    
    outbuf_add(outbuf, buf);
}

void outbuf_fix P1(outbuffer_t *, outbuf) {
    if (outbuf && outbuf->buffer)
	outbuf->buffer = extend_string(outbuf->buffer, outbuf->real_size);
}

void outbuf_push P1(outbuffer_t *, outbuf) {
    (++sp)->type = T_STRING;
    if (outbuf && outbuf->buffer) {
	outbuf->buffer = extend_string(outbuf->buffer, outbuf->real_size);
	
	sp->subtype = STRING_MALLOC;
	sp->u.string = outbuf->buffer;
    } else {
	sp->subtype = STRING_CONSTANT;
	sp->u.string = "";
    }
}
