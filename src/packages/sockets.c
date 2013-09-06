/*
	efuns_sock.c: this file contains the socket efunctions called from
	inside eval_instruction() in interpret.c.
*/

#ifdef LATTICE
#include "/lpc_incl.h"
#include "/socket_err.h"
#include "/include/socket_err.h"
#include "/socket_efuns.h"
#include "/comm.h"
#else
#include "../lpc_incl.h"
#include "../socket_err.h"
#include "../include/socket_err.h"
#include "../socket_efuns.h"
#include "../comm.h"
#endif

#define VALID_SOCKET(x) check_valid_socket((x), fd, get_socket_owner(fd), addr, port)

#ifdef F_SOCKET_CREATE
void
f_socket_create PROT((void))
{
    int fd, num_arg = st_num_arg;
    svalue_t *arg;

    arg = sp - num_arg + 1;
    if ((num_arg == 3) && !(arg[2].type & (T_STRING | T_FUNCTION))) {
	bad_arg(3, F_SOCKET_CREATE);
    }
    if (check_valid_socket("create", -1, current_object, "N/A", -1)) {
	if (num_arg == 2)
	    fd = socket_create(arg[0].u.number, &arg[1], NULL);
	else {
	    fd = socket_create(arg[0].u.number, &arg[1], &arg[2]);
	}
        pop_n_elems(num_arg - 1);
        sp->u.number = fd;
    } else {
        pop_n_elems(num_arg - 1);
        sp->u.number = EESECURITY;
    }
}
#endif

#ifdef F_SOCKET_BIND
void
f_socket_bind PROT((void))
{
    int i, fd, port;
    char addr[ADDR_BUF_SIZE];

    fd = (sp - 1)->u.number;
    get_socket_address(fd, addr, &port);

    if (VALID_SOCKET("bind")) {
	i = socket_bind(fd, sp->u.number);
        (--sp)->u.number = i;
    } else {
	(--sp)->u.number = EESECURITY;
    }
}
#endif

#ifdef F_SOCKET_LISTEN
void
f_socket_listen PROT((void))
{
    int i, fd, port;
    char addr[ADDR_BUF_SIZE];

    fd = (sp - 1)->u.number;
    get_socket_address(fd, addr, &port);

    if (VALID_SOCKET("listen")) {
	i = socket_listen(fd, sp);
	pop_stack();
        sp->u.number = i;
    } else {
	pop_stack();
        sp->u.number = EESECURITY;
    }
}
#endif

#ifdef F_SOCKET_ACCEPT
void
f_socket_accept PROT((void))
{
    int port, fd;
    char addr[ADDR_BUF_SIZE];

    if (!(sp->type & (T_STRING | T_FUNCTION))) {
	bad_arg(3, F_SOCKET_ACCEPT);
    }
    get_socket_address(fd = (sp-2)->u.number, addr, &port);

    (sp-2)->u.number = VALID_SOCKET("accept") ?
       socket_accept(fd, (sp - 1), sp) :
	 EESECURITY;
    pop_2_elems();
}
#endif

#ifdef F_SOCKET_CONNECT
void
f_socket_connect PROT((void))
{
    int i, fd, port;
    char addr[ADDR_BUF_SIZE];

    if (!((sp - 1)->type & (T_FUNCTION | T_STRING))) {
	bad_arg(3, F_SOCKET_CONNECT);
    }
    if (!(sp->type & (T_FUNCTION | T_STRING))) {
	bad_arg(4, F_SOCKET_CONNECT);
    }
    fd = (sp - 3)->u.number;
    get_socket_address(fd, addr, &port);

    if (!strcmp(addr, "0.0.0.0") && port == 0) {
	/*
	 * socket descriptor is not bound yet
	 */
	char *s;
	int start = 0;

	addr[0] = '\0';
	if ((s = strchr((sp - 2)->u.string, ' '))) {
	    /*
	     * use specified address and port
	     */
	    i = s - (sp - 2)->u.string;
	    if (i > ADDR_BUF_SIZE - 1) {
		start = i - ADDR_BUF_SIZE - 1;
		i = ADDR_BUF_SIZE - 1;
	    }
	    strncat(addr, (sp - 2)->u.string + start, i);
	    port = atoi(s + 1);
	}
#ifdef DEBUG
    } else {
	fprintf(stderr, "socket_connect: socket already bound to address/port: %s/%d\n",
		addr, port);
	fprintf(stderr, "socket_connect: requested on: %s\n", (sp - 2)->u.string);
#endif
    }

    (sp-3)->u.number = VALID_SOCKET("connect") ?
      socket_connect(fd, (sp - 2)->u.string, sp - 1, sp) : EESECURITY;
    pop_3_elems();
}
#endif

#ifdef F_SOCKET_WRITE
void
f_socket_write PROT((void))
{
    int i, fd, port;
    svalue_t *arg;
    char addr[ADDR_BUF_SIZE];
    int num_arg = st_num_arg;

    arg = sp - num_arg + 1;
    if ((num_arg == 3) && (arg[2].type != T_STRING)) {
	bad_arg(3, F_SOCKET_WRITE);
    }
    fd = arg[0].u.number;
    get_socket_address(fd, addr, &port);

    if (VALID_SOCKET("write")) {
	i = socket_write(fd, &arg[1],
			 (num_arg == 3) ? arg[2].u.string : (char *) NULL);
        pop_n_elems(num_arg - 1);
        sp->u.number = i;
    } else {
        pop_n_elems(num_arg - 1);
        sp->u.number = EESECURITY;
    }
}
#endif

#ifdef F_SOCKET_CLOSE
void
f_socket_close PROT((void))
{
    int fd, port;
    char addr[ADDR_BUF_SIZE];

    fd = sp->u.number;
    get_socket_address(fd, addr, &port);

    sp->u.number = VALID_SOCKET("close") ? socket_close(fd, 0) : EESECURITY;
}
#endif

#ifdef F_SOCKET_RELEASE
void
f_socket_release PROT((void))
{
    int fd, port;
    char addr[ADDR_BUF_SIZE];
    
    if (!(sp->type & (T_STRING | T_FUNCTION))) {
	bad_arg(3, F_SOCKET_RELEASE);
    }
    fd = (sp - 2)->u.number;
    get_socket_address(fd, addr, &port);

    (sp-2)->u.number = VALID_SOCKET("release") ?
      socket_release((sp - 2)->u.number, (sp - 1)->u.ob, sp) :
	EESECURITY;

    pop_stack();
    /* the object might have been dested an removed from the stack */
    if (sp->type == T_OBJECT)
	free_object(sp->u.ob, "socket_release()");
    sp--;
}
#endif

#ifdef F_SOCKET_ACQUIRE
void
f_socket_acquire PROT((void))
{
    int fd, port;
    char addr[ADDR_BUF_SIZE];

    if (!((sp - 1)->type & (T_FUNCTION | T_STRING))) {
	bad_arg(3, F_SOCKET_ACQUIRE);
    }
    if (!(sp->type & (T_FUNCTION | T_STRING))) {
	bad_arg(4, F_SOCKET_ACQUIRE);
    }
    fd = (sp - 3)->u.number;
    get_socket_address(fd, addr, &port);

    (sp-3)->u.number = VALID_SOCKET("acquire") ?
      socket_acquire((sp - 3)->u.number, (sp - 2),
		     (sp - 1), sp) : EESECURITY;

    pop_3_elems();
}
#endif

#ifdef F_SOCKET_ERROR
void
f_socket_error PROT((void))
{
    put_constant_string(socket_error(sp->u.number));
}
#endif

#ifdef F_SOCKET_ADDRESS
void
f_socket_address PROT((void))
{
    char *str;
    int port;
    char addr[ADDR_BUF_SIZE];
    char buf[2 * ADDR_BUF_SIZE]; /* a bit of overkill to be safe */

/*
 * Ok, we will add in a cute little check thing here to see if it is
 * an object or not...
 */
    if (sp->type & T_OBJECT) {
        char *tmp;

/* This is so we can get the address of interactives as well. */

        if (!sp->u.ob->interactive) {
            free_object(sp->u.ob, "f_socket_address:1");
            *sp = const0u;
            return;
	}
        tmp = inet_ntoa(sp->u.ob->interactive->addr.sin_addr);
        sprintf(buf, "%s %d", tmp, 
		ntohs(sp->u.ob->interactive->addr.sin_port));
	str = string_copy(buf, "f_socket_address");
        free_object(sp->u.ob, "f_socket_address:2");
        put_malloced_string(str);
        return;
    }
    get_socket_address(sp->u.number, addr, &port);
    sprintf(buf, "%s %d", addr, port);
    str = string_copy(buf, "f_socket_address");
    put_malloced_string(str);
}				/* f_socket_address() */
#endif

#ifdef F_DUMP_SOCKET_STATUS
void
f_dump_socket_status PROT((void))
{
    outbuffer_t out;

    outbuf_zero(&out);
    dump_socket_status(&out);
    outbuf_push(&out);
}
#endif


