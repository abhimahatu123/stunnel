/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (c) 1998-2005 Michal Trojnara <Michal.Trojnara@mirt.net>
 *                 All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *   In addition, as a special exception, Michal Trojnara gives
 *   permission to link the code of this program with the OpenSSL
 *   library (or with modified versions of OpenSSL that use the same
 *   license as OpenSSL), and distribute linked combinations including
 *   the two.  You must obey the GNU General Public License in all
 *   respects for all of the code used other than OpenSSL.  If you modify
 *   this file, you may extend this exception to your version of the
 *   file, but you are not obligated to do so.  If you do not wish to
 *   do so, delete this exception statement from your version.
 */

/* Undefine if you have problems with make_sockets() */
#define INET_SOCKET_PAIR

#include "common.h"
#include "prototypes.h"

#ifndef SHUT_RD
#define SHUT_RD 0
#endif
#ifndef SHUT_WR
#define SHUT_WR 1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

/* TCP wrapper */
#ifdef USE_LIBWRAP
#include <tcpd.h>
int allow_severity=LOG_NOTICE;
int deny_severity=LOG_WARNING;
#endif

#if SSLEAY_VERSION_NUMBER >= 0x0922
static unsigned char *sid_ctx=(unsigned char *)"stunnel SID";
    /* const allowed here */
#endif

extern SSL_CTX *ctx; /* global SSL context defined in ssl.c */

static int do_client(CLI *);
static int init_local(CLI *);
static int init_remote(CLI *);
static int init_ssl(CLI *);
static int transfer(CLI *);
static void cleanup(CLI *, int);

static void print_cipher(CLI *);
static int auth_libwrap(CLI *);
static int auth_user(CLI *);
static int connect_local(CLI *);
#ifndef USE_WIN32
static int make_sockets(int [2]);
#endif
static int connect_remote(CLI *c);
static int connect_wait(int, int);
static void reset(int, char *);

int max_clients;
#ifndef USE_WIN32
int max_fds;
#endif

/* Allocate local data structure for the new thread */
void *alloc_client_session(LOCAL_OPTIONS *opt, int rfd, int wfd) {
    CLI *c;

    c=calloc(1, sizeof(CLI));
    if(!c) {
        s_log(LOG_ERR, "Memory allocation failed");
        return NULL;
    }
    c->opt=opt;
    c->local_rfd.fd=rfd;
    c->local_wfd.fd=wfd;
    return c;
}

void *client(void *arg) {
    CLI *c=arg;

#ifdef DEBUG_STACK_SIZE
    stack_info(1); /* initialize */
#endif
    s_log(LOG_DEBUG, "%s started", c->opt->servname);
#ifndef USE_WIN32
    if(c->opt->option.remote && c->opt->option.program)
        c->local_rfd.fd=c->local_wfd.fd=connect_local(c);
            /* connect and exec options specified together */
            /* spawn local program instead of stdio */
#endif
    c->remote_fd.fd=-1;
    c->ssl=NULL;
    cleanup(c, do_client(c));
#ifdef USE_FORK
    if(!c->opt->option.remote) /* 'exec' specified */
        exec_status(); /* null SIGCHLD handler was used */
#else
    enter_critical_section(CRIT_CLIENTS); /* for multi-cpu machines */
    s_log(LOG_DEBUG, "%s finished (%d left)", c->opt->servname,
        --num_clients);
    leave_critical_section(CRIT_CLIENTS);
#endif
    free(c);
#ifdef DEBUG_STACK_SIZE
    stack_info(0); /* display computed value */
#endif
#ifdef USE_WIN32
    _endthread();
#endif
    return NULL;
}

static int do_client(CLI *c) {
    int result;

    if(init_local(c))
        return -1;
    if(!options.option.client && !c->opt->protocol) {
        /* Server mode and no protocol negotiation needed */
        if(init_ssl(c))
            return -1;
        if(init_remote(c))
            return -1;
    } else {
        if(init_remote(c))
            return -1;
        if(negotiate(c)<0) {
            s_log(LOG_ERR, "Protocol negotiations failed");
            return -1;
        }
        if(init_ssl(c))
            return -1;
    }
    result=transfer(c);
    s_log(LOG_NOTICE,
        "Connection %s: %d bytes sent to SSL, %d bytes sent to socket",
         result ? "reset" : "closed", c->ssl_bytes, c->sock_bytes);
    return result;
}

static int init_local(CLI *c) {
    SOCKADDR_UNION addr;
    int addrlen;

    addrlen=sizeof(SOCKADDR_UNION);
    if(getpeername(c->local_rfd.fd, &addr.sa, &addrlen)<0) {
        strcpy(c->accepting_address, "NOT A SOCKET");
        c->local_rfd.is_socket=0;
        c->local_wfd.is_socket=0; /* TODO: It's not always true */
#ifdef USE_WIN32
        if(get_last_socket_error()!=ENOTSOCK) {
#else
        if(c->opt->option.transparent || get_last_socket_error()!=ENOTSOCK) {
#endif
            sockerror("getpeerbyname");
            return -1;
        }
        /* Ignore ENOTSOCK error so 'local' doesn't have to be a socket */
    } else { /* success */
        /* copy addr to c->peer_addr */
        memcpy(&c->peer_addr.addr[0], &addr, sizeof(SOCKADDR_UNION));
        c->peer_addr.num=1;
        s_ntop(c->accepting_address, &c->peer_addr.addr[0]);
        c->local_rfd.is_socket=1;
        c->local_wfd.is_socket=1; /* TODO: It's not always true */
        /* It's a socket: lets setup options */
        if(set_socket_options(c->local_rfd.fd, 1)<0)
            return -1;
        if(auth_libwrap(c)<0)
            return -1;
        if(auth_user(c)<0) {
            s_log(LOG_WARNING, "Connection from %s REFUSED by IDENT",
                c->accepting_address);
            return -1;
        }
        s_log(LOG_NOTICE, "%s connected from %s",
            c->opt->servname, c->accepting_address);
    }
    return 0; /* OK */
}

static int init_remote(CLI *c) {
    int fd;

    /* create connection to host/service */
    if(c->opt->source_addr.num)
        memcpy(&c->bind_addr, &c->opt->source_addr, sizeof(SOCKADDR_LIST));
#ifndef USE_WIN32
    else if(c->opt->option.transparent)
        memcpy(&c->bind_addr, &c->peer_addr, sizeof(SOCKADDR_LIST));
#endif
    else {
        c->bind_addr.num=0; /* don't bind connecting socket */
    }
    /* Setup c->remote_fd, now */
    if(c->opt->option.remote) {
        fd=connect_remote(c);
    } else /* NOT in remote mode */
        fd=connect_local(c);
    if(fd<0) {
        s_log(LOG_ERR, "Failed to initialize remote connection");
        return -1;
    }
#ifndef USE_WIN32
    if(fd>=max_fds) {
        s_log(LOG_ERR, "Remote file descriptor out of range (%d>=%d)",
            fd, max_fds);
        closesocket(fd);
        return -1;
    }
#endif
    s_log(LOG_DEBUG, "Remote FD=%d initialized", fd);
    c->remote_fd.fd=fd;
    c->remote_fd.is_socket=1; /* Always! */
    if(set_socket_options(fd, 2)<0)
        return -1;
    return 0; /* OK */
}

static int init_ssl(CLI *c) {
    int i, err;
    s_poll_set fds;
    SSL_SESSION *old_session;

    if(!(c->ssl=SSL_new(ctx))) {
        sslerror("SSL_new");
        return -1;
    }
#if SSLEAY_VERSION_NUMBER >= 0x0922
    SSL_set_session_id_context(c->ssl, sid_ctx, strlen(sid_ctx));
#endif
    if(options.option.client) {
        if(c->opt->session) {
            enter_critical_section(CRIT_SESSION);
            SSL_set_session(c->ssl, c->opt->session);
            leave_critical_section(CRIT_SESSION);
        }
        SSL_set_fd(c->ssl, c->remote_fd.fd);
        SSL_set_connect_state(c->ssl);
    } else {
        if(c->local_rfd.fd==c->local_wfd.fd)
            SSL_set_fd(c->ssl, c->local_rfd.fd);
        else {
           /* Does it make sence to have SSL on STDIN/STDOUT? */
            SSL_set_rfd(c->ssl, c->local_rfd.fd);
            SSL_set_wfd(c->ssl, c->local_wfd.fd);
        }
        SSL_set_accept_state(c->ssl);
    }

    /* Setup some values for transfer() function */
    if(options.option.client) {
        c->sock_rfd=&(c->local_rfd);
        c->sock_wfd=&(c->local_wfd);
        c->ssl_rfd=c->ssl_wfd=&(c->remote_fd);
    } else {
        c->sock_rfd=c->sock_wfd=&(c->remote_fd);
        c->ssl_rfd=&(c->local_rfd);
        c->ssl_wfd=&(c->local_wfd);
    }

    while(1) {
        if(options.option.client)
            i=SSL_connect(c->ssl);
        else
            i=SSL_accept(c->ssl);
        err=SSL_get_error(c->ssl, i);
        if(err==SSL_ERROR_NONE)
            break; /* ok -> done */
        if(err==SSL_ERROR_WANT_READ || err==SSL_ERROR_WANT_WRITE) {
            s_poll_zero(&fds);
            s_poll_add(&fds, c->ssl_rfd->fd,
                err==SSL_ERROR_WANT_READ,
                err==SSL_ERROR_WANT_WRITE);
            switch(s_poll_wait(&fds, c->opt->timeout_busy)) {
            case -1:
                sockerror("init_ssl: s_poll_wait");
                return -1; /* error */
            case 0:
                s_log(LOG_INFO, "init_ssl: s_poll_wait timeout");
                return -1; /* timeout */
            case 1:
                break; /* OK */
            default:
                s_log(LOG_ERR, "init_ssl: s_poll_wait unknown result");
                return -1; /* error */
            }
            continue; /* ok -> retry */
        }
        if(err==SSL_ERROR_SYSCALL) {
            switch(get_last_socket_error()) {
            case EINTR:
            case EAGAIN:
                continue;
            }
        }
        if(options.option.client)
            sslerror("SSL_connect");
        else
            sslerror("SSL_accept");
        return -1;
    }
    if(SSL_session_reused(c->ssl)) {
        if(options.option.client)
            s_log(LOG_INFO, "SSL connected: previous session reused");
        else
            s_log(LOG_INFO, "SSL accepted: previous session reused");
    } else { /* a new session was negotiated */
        if(options.option.client) {
            s_log(LOG_INFO, "SSL connected: new session negotiated");
            enter_critical_section(CRIT_SESSION);
            old_session=c->opt->session;
            c->opt->session=SSL_get1_session(c->ssl); /* store it */
            if(old_session)
                SSL_SESSION_free(old_session); /* release the old one */
            leave_critical_section(CRIT_SESSION);
        } else
            s_log(LOG_INFO, "SSL accepted: new session negotiated");
        print_cipher(c);
    }
    return 0; /* OK */
}

/* open file descriptors for transfer() */
#define sock_rd (c->sock_rfd->rd)
#define sock_wr (c->sock_wfd->wr)
#define ssl_rd  (c->ssl_rfd->rd)
#define ssl_wr  (c->ssl_wfd->wr)

/* ready file descriptors for transfer() */
#define sock_can_rd (s_poll_canread(&fds, c->sock_rfd->fd))
#define sock_can_wr (s_poll_canwrite(&fds, c->sock_wfd->fd))
#define ssl_can_rd  (s_poll_canread(&fds, c->ssl_rfd->fd))
#define ssl_can_wr  (s_poll_canwrite(&fds, c->ssl_wfd->fd))

static int transfer(CLI *c) { /* transfer data */
    s_poll_set fds;
    int num, err;
    int check_SSL_pending;
    int ssl_closing=0;
        /* 0=not closing SSL, 1=initiate SSL_shutdown,
         * 2=retry SSL_shutdown, 3=SSL_shutdown done */
    int watchdog=0; /* a counter to detect an infinite loop */

    c->sock_ptr=c->ssl_ptr=0;
    sock_rd=sock_wr=ssl_rd=ssl_wr=1;
    c->sock_bytes=c->ssl_bytes=0;

    while(((sock_rd||c->sock_ptr)&&ssl_wr)||((ssl_rd||c->ssl_ptr)&&sock_wr)) {
        s_poll_zero(&fds); /* Initialize the structure */

        if(sock_rd && c->sock_ptr<BUFFSIZE) /* socket input buffer not full*/
            s_poll_add(&fds, c->sock_rfd->fd, 1, 0);
        if(ssl_rd && (
                c->ssl_ptr<BUFFSIZE || /* SSL input buffer not full */
                ((c->sock_ptr||ssl_closing) && SSL_want_read(c->ssl))
                /* I want to SSL_write or SSL_shutdown but read from the
                 * underlying socket needed for the SSL protocol */
                )) {
            s_poll_add(&fds, c->ssl_rfd->fd, 1, 0);
        }

        if(sock_wr && c->ssl_ptr) /* SSL input buffer not empty */
            s_poll_add(&fds, c->sock_wfd->fd, 0, 1);
        if (ssl_wr && (
                c->sock_ptr || /* socket input buffer not empty */
                ssl_closing==1 || /* initiate SSL_shutdown */
                ((c->ssl_ptr<BUFFSIZE || ssl_closing==2) &&
                    SSL_want_write(c->ssl))
                /* I want to SSL_read or SSL_shutdown but write to the
                 * underlying socket needed for the SSL protocol */
                )) {
            s_poll_add(&fds, c->ssl_wfd->fd, 0, 1);
        }

        switch(s_poll_wait(&fds,
            sock_rd || (ssl_wr&&c->sock_ptr) || (sock_wr&&c->ssl_ptr) ?
            c->opt->timeout_idle : c->opt->timeout_close)) {
        case -1:
            sockerror("s_poll_wait");
            return -1;
        case 0: /* Timeout */
            if(sock_rd) { /* No traffic for a long time */
                s_log(LOG_DEBUG, "s_poll_wait timeout: connection reset");
                return -1;
            } else { /* Timeout waiting for SSL close_notify */
                s_log(LOG_DEBUG,
                    "s_poll_wait timeout waiting for SSL close_notify");
                return 0; /* OK */
            }
        }

        if(ssl_closing==1 /* initiate SSL_shutdown */ || (ssl_closing==2 && (
                (SSL_want_read(c->ssl) && ssl_can_rd) ||
                (SSL_want_write(c->ssl) && ssl_can_wr)
                ))) {
            switch(SSL_shutdown(c->ssl)) { /* Send close_notify */
            case 1: /* the shutdown was successfully completed */
                s_log(LOG_INFO, "SSL_shutdown successfully sent close_notify");
                ssl_wr=0; /* SSL write closed */
                /* TODO: It's not really closed.  We need to distinguish
                 * closed SSL and closed underlying file descriptor */
                ssl_closing=3; /* done! */
                break;
            case 0: /* the shutdown is not yet finished */
                s_log(LOG_DEBUG, "SSL_shutdown retrying");
                ssl_closing=2; /* next time just retry SSL_shutdown */
                break;
            case -1: /* a fatal error occurred */
                sslerror("SSL_shutdown");
                return -1;
            }
        }

        /* Set flag to try and read any buffered SSL data if we made */
        /* room in the buffer by writing to the socket */
        check_SSL_pending = 0;

        if(sock_wr && sock_can_wr) {
            switch(num=writesocket(c->sock_wfd->fd, c->ssl_buff, c->ssl_ptr)) {
            case -1: /* error */
                switch(get_last_socket_error()) {
                case EINTR:
                    s_log(LOG_DEBUG,
                        "writesocket interrupted by a signal: retrying");
                    break;
                case EWOULDBLOCK:
                    s_log(LOG_NOTICE, "writesocket would block: retrying");
                    break;
                default:
                    sockerror("writesocket");
                    return -1;
                }
                break;
            case 0:
                s_log(LOG_DEBUG, "No data written to the socket: retrying");
                break;
            default:
                memmove(c->ssl_buff, c->ssl_buff+num, c->ssl_ptr-num);
                if(c->ssl_ptr==BUFFSIZE)
                    check_SSL_pending=1;
                c->ssl_ptr-=num;
                c->sock_bytes+=num;
                watchdog=0; /* reset watchdog */
                if(!ssl_rd && !c->ssl_ptr) {
                    shutdown(c->sock_wfd->fd, SHUT_WR);
                    s_log(LOG_DEBUG,
                        "Socket write shutdown (no more data to send)");
                    sock_wr=0;
                }
            }
        }

        if(ssl_wr && ( /* SSL sockets are still open */
                (c->sock_ptr && ssl_can_wr) ||
                /* See if application data can be written */
                (SSL_want_read(c->ssl) && ssl_can_rd)
                /* I want to SSL_write but read from the underlying */
                /* socket needed for the SSL protocol */
                )) {
            num=SSL_write(c->ssl, c->sock_buff, c->sock_ptr);

            switch(err=SSL_get_error(c->ssl, num)) {
            case SSL_ERROR_NONE:
                memmove(c->sock_buff, c->sock_buff+num, c->sock_ptr-num);
                c->sock_ptr-=num;
                c->ssl_bytes+=num;
                watchdog=0; /* reset watchdog */
                if(!ssl_closing && !sock_rd && !c->sock_ptr && ssl_wr) {
                    s_log(LOG_DEBUG,
                        "SSL write shutdown (no more data to send)");
                    ssl_closing=1;
                }
                break;
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_X509_LOOKUP:
                s_log(LOG_DEBUG, "SSL_write returned WANT_: retrying");
                break;
            case SSL_ERROR_SYSCALL:
                if(num<0) { /* really an error */
                    switch(get_last_socket_error()) {
                    case EINTR:
                        s_log(LOG_DEBUG,
                            "SSL_write interrupted by a signal: retrying");
                        break;
                    case EAGAIN:
                        s_log(LOG_DEBUG,
                            "SSL_write returned EAGAIN: retrying");
                        break;
                    default:
                        sockerror("SSL_write (ERROR_SYSCALL)");
                        return -1;
                    }
                }
                break;
            case SSL_ERROR_ZERO_RETURN: /* close_notify received */
                s_log(LOG_DEBUG, "SSL closed on SSL_write");
                ssl_rd=ssl_wr=0;
                break;
            case SSL_ERROR_SSL:
                sslerror("SSL_write");
                return -1;
            default:
                s_log(LOG_ERR, "SSL_write/SSL_get_error returned %d", err);
                return -1;
            }
        }

        if(sock_rd && sock_can_rd) {
            switch(num=readsocket(c->sock_rfd->fd,
                c->sock_buff+c->sock_ptr, BUFFSIZE-c->sock_ptr)) {
            case -1:
                switch(get_last_socket_error()) {
                case EINTR:
                    s_log(LOG_DEBUG,
                        "readsocket interrupted by a signal: retrying");
                    break;
                case EWOULDBLOCK:
                    s_log(LOG_NOTICE, "readsocket would block: retrying");
                    break;
                default:
                    sockerror("readsocket");
                    return -1;
                }
                break;
            case 0: /* close */
                s_log(LOG_DEBUG, "Socket closed on read");
                sock_rd=0;
                if(!ssl_closing && !c->sock_ptr && ssl_wr) {
                    s_log(LOG_DEBUG,
                        "SSL write shutdown (output buffer empty)");
                    ssl_closing=1;
                }
                break;
            default:
                c->sock_ptr+=num;
                watchdog=0; /* reset watchdog */
            }
        }

        if(ssl_rd && ( /* SSL sockets are still open */
                (c->ssl_ptr<BUFFSIZE && ssl_can_rd) ||
                /* See if there's any application data coming in */
                (SSL_want_write(c->ssl) && ssl_can_wr) ||
                /* I want to SSL_read but write to the underlying */
                /* socket needed for the SSL protocol */
                (check_SSL_pending && SSL_pending(c->ssl))
                /* Write made space from full buffer */
                )) {
            num=SSL_read(c->ssl, c->ssl_buff+c->ssl_ptr, BUFFSIZE-c->ssl_ptr);

            switch(err=SSL_get_error(c->ssl, num)) {
            case SSL_ERROR_NONE:
                c->ssl_ptr+=num;
                watchdog=0; /* reset watchdog */
                break;
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_X509_LOOKUP:
                s_log(LOG_DEBUG, "SSL_read returned WANT_: retrying");
                break;
            case SSL_ERROR_SYSCALL:
                if(num<0) { /* not EOF */
                    switch(get_last_socket_error()) {
                    case EINTR:
                        s_log(LOG_DEBUG,
                            "SSL_read interrupted by a signal: retrying");
                        break;
                    case EAGAIN:
                        s_log(LOG_DEBUG,
                            "SSL_read returned EAGAIN: retrying");
                        break;
                    default:
                        sockerror("SSL_read (ERROR_SYSCALL)");
                        return -1;
                    }
                } else { /* EOF */
                    s_log(LOG_DEBUG, "SSL socket closed on SSL_read");
                    ssl_rd=ssl_wr=0;
                }
                break;
            case SSL_ERROR_ZERO_RETURN: /* close_notify received */
                s_log(LOG_DEBUG, "SSL closed on SSL_read");
                ssl_rd=0;
                if(!ssl_closing && !c->sock_ptr && ssl_wr) {
                    s_log(LOG_DEBUG,
                        "SSL write shutdown (output buffer empty)");
                    ssl_closing=1;
                }
                if(!c->ssl_ptr && sock_wr) {
                    shutdown(c->sock_wfd->fd, SHUT_WR);
                    s_log(LOG_DEBUG,
                        "Socket write shutdown (output buffer empty)");
                    sock_wr=0;
                }
                break;
            case SSL_ERROR_SSL:
                sslerror("SSL_read");
                return -1;
            default:
                s_log(LOG_ERR, "SSL_read/SSL_get_error returned %d", err);
                return -1;
            }
        }

        if(++watchdog>1000) { /* loop executes not transferring any data */
            s_log(LOG_ERR,
                "transfer() loop executes not transferring any data");
            s_log(LOG_ERR,
                "please report the problem to Michal.Trojnara@mirt.net");
            s_log(LOG_ERR, "socket open rd=%s wr=%s, ssl open rd=%s wr=%s",
                sock_rd ? "yes" : "no", sock_wr ? "yes" : "no",
                ssl_rd ? "yes" : "no", ssl_wr ? "yes" : "no");
            s_log(LOG_ERR, "socket ready rd=%s wr=%s, ssl ready rd=%s wr=%s",
                sock_can_rd ? "yes" : "no", sock_can_wr ? "yes" : "no",
                ssl_can_rd ? "yes" : "no", ssl_can_wr ? "yes" : "no");
            s_log(LOG_ERR, "check_SSL_pending=%d, ssl_closing=%d",
                check_SSL_pending, ssl_closing);

            return -1;
        }

    } /* while */

    return 0; /* OK */
}

static void cleanup(CLI *c, int error) {
        /* Cleanup SSL */
    if(c->ssl) { /* SSL initialized */
        SSL_set_shutdown(c->ssl, SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN);
        SSL_free(c->ssl);
        ERR_remove_state(0);
    }
        /* Cleanup remote socket */
    if(c->remote_fd.fd>=0) { /* Remote socket initialized */
        if(error && c->remote_fd.is_socket)
            reset(c->remote_fd.fd, "linger (remote)");
        closesocket(c->remote_fd.fd);
    }
        /* Cleanup local socket */
    if(c->local_rfd.fd>=0) { /* Local socket initialized */
        if(c->local_rfd.fd==c->local_wfd.fd) {
            if(error && c->local_rfd.is_socket)
                reset(c->local_rfd.fd, "linger (local)");
            closesocket(c->local_rfd.fd);
        } else { /* STDIO */
            if(error && c->local_rfd.is_socket)
                reset(c->local_rfd.fd, "linger (local_rfd)");
            if(error && c->local_wfd.is_socket)
                reset(c->local_wfd.fd, "linger (local_wfd)");
       }
    }
}

static void print_cipher(CLI *c) { /* print negotiated cipher */
#if SSLEAY_VERSION_NUMBER <= 0x0800
    s_log(LOG_INFO, "%s opened with SSLv%d, cipher %s",
        c->opt->servname, ssl->session->ssl_version, SSL_get_cipher(c->ssl));
#else
    SSL_CIPHER *cipher;
    char buf[STRLEN];
    int len;

    cipher=SSL_get_current_cipher(c->ssl);
    SSL_CIPHER_description(cipher, buf, STRLEN);
    len=strlen(buf);
    if(len>0)
        buf[len-1]='\0';
    s_log(LOG_INFO, "Negotiated ciphers: %s", buf);
#endif
}

static int auth_libwrap(CLI *c) {
#ifdef USE_LIBWRAP
    struct request_info request;
    int result;

    enter_critical_section(CRIT_INET); /* libwrap is not mt-safe */
    request_init(&request,
        RQ_DAEMON, c->opt->servname, RQ_FILE, c->local_rfd.fd, 0);
    fromhost(&request);
    result=hosts_access(&request);
    leave_critical_section(CRIT_INET);

    if (!result) {
        s_log(LOG_WARNING, "Connection from %s REFUSED by libwrap",
            c->accepting_address);
        s_log(LOG_DEBUG, "See hosts_access(5) for details");
        return -1; /* FAILED */
    }
#endif
    return 0; /* OK */
}

static int auth_user(CLI *c) {
    struct servent *s_ent;    /* structure for getservbyname */
    SOCKADDR_UNION ident;     /* IDENT socket name */
    int fd;                   /* IDENT socket descriptor */
    char name[STRLEN];
    int retval;
    int error;

    if(!c->opt->username)
        return 0; /* -u option not specified */
    if((fd=socket(c->peer_addr.addr[0].sa.sa_family, SOCK_STREAM, 0))<0) {
        sockerror("socket (auth_user)");
        return -1;
    }
    if(alloc_fd(fd))
        return -1;
    memcpy(&ident, &c->peer_addr.addr[0], sizeof(SOCKADDR_UNION));
    s_ent=getservbyname("auth", "tcp");
    if(s_ent) {
        ident.in.sin_port=s_ent->s_port;
    } else {
        s_log(LOG_WARNING, "Unknown service 'auth': using default 113");
        ident.in.sin_port=htons(113);
    }
    if(connect(fd, &ident.sa, addr_len(ident))) {
        error=get_last_socket_error();
        if(error!=EINPROGRESS && error!=EWOULDBLOCK) {
            sockerror("ident connect (auth_user)");
            closesocket(fd);
            return -1;
        }
        if(connect_wait(fd, c->opt->timeout_connect)) { /* error */
            closesocket(fd);
            return -1;
        }
    }
    s_log(LOG_DEBUG, "IDENT server connected");
    if(fdprintf(c, fd, "%u , %u",
            ntohs(c->peer_addr.addr[0].in.sin_port),
            ntohs(c->opt->local_addr.addr[0].in.sin_port))<0) {
        sockerror("fdprintf (auth_user)");
        closesocket(fd);
        return -1;
    }
    if(fdscanf(c, fd, "%*[^:]: USERID :%*[^:]:%s", name)!=1) {
        s_log(LOG_ERR, "Incorrect data from IDENT server");
        closesocket(fd);
        return -1;
    }
    closesocket(fd);
    retval=strcmp(name, c->opt->username) ? -1 : 0;
    safestring(name);
    s_log(LOG_INFO, "IDENT resolved remote user to %s", name);
    return retval;
}

static int connect_local(CLI *c) { /* spawn local process */
#if defined (USE_WIN32) || defined (__vms)
    s_log(LOG_ERR, "LOCAL MODE NOT SUPPORTED ON WIN32 and OpenVMS PLATFORM");
    return -1;
#else /* USE_WIN32, __vms */
    char env[3][STRLEN], name[STRLEN], *portname;
    int fd[2], pid;
    X509 *peer;
#ifdef HAVE_PTHREAD_SIGMASK
    sigset_t newmask;
#endif

    if (c->opt->option.pty) {
        char tty[STRLEN];

        if(pty_allocate(fd, fd+1, tty, STRLEN)) {
            return -1;
        }
        s_log(LOG_DEBUG, "%s allocated", tty);
    } else {
        if(make_sockets(fd))
            return -1;
    }
    pid=fork();
    c->pid=(unsigned long)pid;
    switch(pid) {
    case -1:    /* error */
        closesocket(fd[0]);
        closesocket(fd[1]);
        ioerror("fork");
        return -1;
    case  0:    /* child */
        closesocket(fd[0]);
        dup2(fd[1], 0);
        dup2(fd[1], 1);
        if(!options.option.foreground)
            dup2(fd[1], 2);
        closesocket(fd[1]);
        safecopy(env[0], "REMOTE_HOST=");
        safeconcat(env[0], c->accepting_address);
        portname=strrchr(env[0], ':');
        if(portname) /* strip the port name */
            *portname='\0';
        putenv(env[0]);
        if(c->opt->option.transparent) {
            putenv("LD_PRELOAD=" LIBDIR "/libstunnel.so");
            /* For Tru64 _RLD_LIST is used instead */
            putenv("_RLD_LIST=" LIBDIR "/libstunnel.so:DEFAULT");
        }
        if(c->ssl) {
            peer=SSL_get_peer_certificate(c->ssl);
            if(peer) {
                safecopy(env[1], "SSL_CLIENT_DN=");
                X509_NAME_oneline(X509_get_subject_name(peer), name, STRLEN);
                safestring(name);
                safeconcat(env[1], name);
                putenv(env[1]);
                safecopy(env[2], "SSL_CLIENT_I_DN=");
                X509_NAME_oneline(X509_get_issuer_name(peer), name, STRLEN);
                safestring(name);
                safeconcat(env[2], name);
                putenv(env[2]);
                X509_free(peer);
            }
        }
#ifdef HAVE_PTHREAD_SIGMASK
        sigemptyset(&newmask);
        sigprocmask(SIG_SETMASK, &newmask, NULL);
#endif
        execvp(c->opt->execname, c->opt->execargs);
        ioerror(c->opt->execname); /* execv failed */
        _exit(1);
    default:
        break;
    }
    /* parent */
    s_log(LOG_INFO, "Local mode child started (PID=%lu)", c->pid);
    closesocket(fd[1]);
#ifdef FD_CLOEXEC
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
#endif
    return fd[0];
#endif /* USE_WIN32,__vms */
}

#ifndef USE_WIN32

static int make_sockets(int fd[2]) { /* make a pair of connected sockets */
#ifdef INET_SOCKET_PAIR
    SOCKADDR_UNION addr;
    int addrlen;
    int s; /* temporary socket awaiting for connection */

    if((s=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket#1");
        return -1;
    }
    if((fd[1]=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket#2");
        return -1;
    }
    addrlen=sizeof(SOCKADDR_UNION);
    memset(&addr, 0, addrlen);
    addr.in.sin_family=AF_INET;
    addr.in.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    addr.in.sin_port=0; /* dynamic port allocation */
    if(bind(s, &addr.sa, addrlen))
        log_error(LOG_DEBUG, get_last_socket_error(), "bind#1");
    if(bind(fd[1], &addr.sa, addrlen))
        log_error(LOG_DEBUG, get_last_socket_error(), "bind#2");
    if(listen(s, 5)) {
        sockerror("listen");
        return -1;
    }
    if(getsockname(s, &addr.sa, &addrlen)) {
        sockerror("getsockname");
        return -1;
    }
    if(connect(fd[1], &addr.sa, addrlen)) {
        sockerror("connect");
        return -1;
    }
    if((fd[0]=accept(s, &addr.sa, &addrlen))<0) {
        sockerror("accept");
        return -1;
    }
    closesocket(s); /* don't care about the result */
#else
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) {
        sockerror("socketpair");
        return -1;
    }
#endif
    return 0;
}
#endif

static int connect_remote(CLI *c) { /* connect to remote host */
    SOCKADDR_UNION bind_addr, addr;
    SOCKADDR_LIST resolved_list, *address_list;
    int error;
    int s; /* destination socket */
    u16 i;

    /* setup address_list */
    if(c->opt->option.delayed_lookup) {
        resolved_list.num=0;
        if(!name2addrlist(&resolved_list,
                c->opt->remote_address, DEFAULT_LOOPBACK))
            return -1; /* no host resolved */
        address_list=&resolved_list;
    } else /* use pre-resolved addresses */
        address_list=&c->opt->remote_addr;

    /* try to connect each host from the list */
    for(i=0; i<address_list->num; i++) {
        memcpy(&addr, address_list->addr + address_list->cur,
            sizeof(SOCKADDR_UNION));
        address_list->cur=(address_list->cur+1)%address_list->num;
        /* race condition is possible, but harmless in this case */

        if((s=socket(addr.sa.sa_family, SOCK_STREAM, 0))<0) {
            sockerror("remote socket");
            return -1;
        }
        if(alloc_fd(s))
            return -1;

        if(c->bind_addr.num) { /* explicit local bind or transparent proxy */
            memcpy(&bind_addr, &c->bind_addr.addr[0], sizeof(SOCKADDR_UNION));
            if(bind(s, &bind_addr.sa, addr_len(bind_addr))<0) {
                sockerror("bind transparent");
                closesocket(s);
                return -1;
            }
        }

        /* try to connect for the 1st time */
        s_ntop(c->connecting_address, &addr);
        s_log(LOG_DEBUG, "%s connecting %s",
            c->opt->servname, c->connecting_address);
        if(!connect(s, &addr.sa, addr_len(addr)))
            return s; /* no error -> success (should not be possible) */
        error=get_last_socket_error();
        if(error!=EINPROGRESS && error!=EWOULDBLOCK) {
            s_log(LOG_ERR, "remote connect (%s): %s (%d)",
                c->connecting_address, my_strerror(error), error);
            closesocket(s);
            continue; /* next IP */
        }
        if(!connect_wait(s, c->opt->timeout_connect))
            return s; /* success! */
        closesocket(s); /* error -> next IP */
    }
    return -1;
}

    /* wait for the result of a non-blocking connect */
static int connect_wait(int fd, int timeout) {
    s_poll_set fds;
    int error, len;

    s_log(LOG_DEBUG, "connect_wait: waiting %d seconds", timeout);
    s_poll_zero(&fds);
    s_poll_add(&fds, fd, 1, 1);
    switch(s_poll_wait(&fds, timeout)) {
    case -1:
        sockerror("connect_wait: s_poll_wait");
        return -1; /* error */
    case 0:
        s_log(LOG_INFO, "connect_wait: s_poll_wait timeout");
        return -1; /* error */
    default:
        if(s_poll_canread(&fds, fd)) {
            /* just connected socket should not be ready for read */
            /* get the resulting error code, now */
            len=sizeof(error);
            if(!getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&error, &len))
                errno=error;
            if(errno) { /* really an error? */
                sockerror("connect_wait: getsockopt");
                return -1; /* connection failed */
            }
        }
        if(s_poll_canwrite(&fds, fd)) {
            s_log(LOG_DEBUG, "connect_wait: connected");
            return 0; /* success */
        }
        s_log(LOG_ERR, "connect_wait: unexpected s_poll_wait result");
        return -1; /* unexpected result */
    }
    return -1; /* should not be possible */
}

static void reset(int fd, char *txt) {
    /* Set lingering on a socket if needed*/
    struct linger l;

    l.l_onoff=1;
    l.l_linger=0;
    if(setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof(l)))
        log_error(LOG_DEBUG, get_last_socket_error(), txt);
}

/* End of client.c */
