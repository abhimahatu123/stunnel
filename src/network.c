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

#include "common.h"
#include "prototypes.h"

#ifdef USE_FORK
static void client_status(void); /* dead children detected */
#endif
 
#ifndef USE_WIN32

static int signal_pipe[2];
static char signal_buffer[16];

static void sigchld_handler(int sig) { /* SIGCHLD detected */
    int save_errno=errno;

    write(signal_pipe[1], signal_buffer, 1);
    signal(SIGCHLD, sigchld_handler);
    errno=save_errno;
}

int signal_pipe_init(void) {
    if(pipe(signal_pipe)) {
        ioerror("pipe");
        exit(1);
    }
    alloc_fd(signal_pipe[0]);
    alloc_fd(signal_pipe[1]);
#ifdef FD_CLOEXEC
    /* close the pipe in child execvp */
    fcntl(signal_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(signal_pipe[1], F_SETFD, FD_CLOEXEC);
#endif
    signal(SIGCHLD, sigchld_handler);
    return signal_pipe[0];
}

static void signal_pipe_empty(void) {
    read(signal_pipe[0], signal_buffer, sizeof(signal_buffer));
#ifdef USE_PTHREAD
    exec_status(); /* report status of 'exec' process */
#endif /* USE_PTHREAD */
#ifdef USE_FORK
    client_status(); /* report status of client process */
#endif /* USE_FORK */
}

#endif /* !defined(USE_WIN32) */

#ifdef HAVE_POLL

void s_poll_zero(s_poll_set *fds) {
    fds->nfds=0;
}

void s_poll_add(s_poll_set *fds, int fd, int rd, int wr) {
    int i;

    for(i=0; i<fds->nfds && fds->ufds[i].fd!=fd; i++)
        ;
    if(i>=MAX_FD) {
        s_log(LOG_ERR,
            "s_poll_add failed for FD=%d: too many file descriptors", fd);
        return;
    }
    if(i==fds->nfds) {
        fds->ufds[i].fd=fd;
        fds->ufds[i].events=0;
        fds->nfds++;
    }
    if(rd)
        fds->ufds[i].events|=POLLIN;
    if(wr)
        fds->ufds[i].events|=POLLOUT;
}

int s_poll_canread(s_poll_set *fds, int fd) {
    int i;

    for(i=0; i<fds->nfds; i++)
        if(fds->ufds[i].fd==fd)
            return fds->ufds[i].revents&POLLIN;
    return 0;
}

int s_poll_canwrite(s_poll_set *fds, int fd) {
    int i;

    for(i=0; i<fds->nfds; i++)
        if(fds->ufds[i].fd==fd)
            return fds->ufds[i].revents&POLLOUT;
    return 0;
}

int s_poll_wait(s_poll_set *fds, int timeout) {
    int retval, retry;

    do { /* skip "Interrupted system call" errors */
        retry=0;
        retval=poll(fds->ufds, fds->nfds, timeout<0 ? -1 : 1000*timeout);
        if(timeout<0 && retval>0 && s_poll_canread(fds, signal_pipe[0])) {
            signal_pipe_empty(); /* no timeout -> main loop */
            retry=1;
        }
    } while(retry || (retval<0 && get_last_socket_error()==EINTR));
    return retval;
}

#else /* HAVE_POLL */

void s_poll_zero(s_poll_set *fds) {
    FD_ZERO(&fds->irfds);
    FD_ZERO(&fds->iwfds);
    fds->max = 0; /* No file descriptors */
}

void s_poll_add(s_poll_set *fds, int fd, int rd, int wr) {
    if(rd)
        FD_SET(fd, &fds->irfds);
    if(wr)
        FD_SET(fd, &fds->iwfds);
    if(fd > fds->max)
        fds->max = fd;
}

int s_poll_canread(s_poll_set *fds, int fd) {
    return FD_ISSET(fd, &fds->orfds);
}

int s_poll_canwrite(s_poll_set *fds, int fd) {
    return FD_ISSET(fd, &fds->owfds);
}

int s_poll_wait(s_poll_set *fds, int timeout) {
    int retval, retry;
    struct timeval tv, *tv_ptr;

    do { /* skip "Interrupted system call" errors */
        retry=0;
        memcpy(&fds->orfds, &fds->irfds, sizeof(fd_set));
        memcpy(&fds->owfds, &fds->iwfds, sizeof(fd_set));
        if(timeout<0) { /* infinite timeout */
            tv_ptr=NULL;
        } else {
            tv.tv_sec=timeout;
            tv.tv_usec=0;
            tv_ptr=&tv;
        }
        retval=select(fds->max+1, &fds->orfds, &fds->owfds, NULL, tv_ptr);
#ifndef USE_WIN32
        if(timeout<0 && retval>0 && s_poll_canread(fds, signal_pipe[0])) {
            signal_pipe_empty(); /* no timeout -> main loop */
            retry=1;
        }
#endif
    } while(retry || (retval<0 && get_last_socket_error()==EINTR));
    return retval;
}

#endif /* HAVE_POLL */

#ifdef USE_FORK
static void client_status(void) { /* dead children detected */
    int pid, status;

#ifdef HAVE_WAIT_FOR_PID
    while((pid=wait_for_pid(-1, &status, WNOHANG))>0) {
        num_clients--; /* one client less */
#else
    if((pid=wait(&status))>0) {
        num_clients--; /* one client less */
#endif
#ifdef WIFSIGNALED
        if(WIFSIGNALED(status)) {
            s_log(LOG_DEBUG, "Process %d terminated on signal %d (%d left)",
                pid, WTERMSIG(status), num_clients);
        } else {
            s_log(LOG_DEBUG, "Process %d finished with code %d (%d left)",
                pid, WEXITSTATUS(status), num_clients);
        }
    }
#else
        s_log(LOG_DEBUG, "Process %d finished with code %d (%d left)",
            pid, status, num_clients);
    }
#endif
}
#endif

#ifndef USE_WIN32
void exec_status(void) { /* dead local ('exec') process detected */
    int pid, status;

#ifdef HAVE_WAIT_FOR_PID
    while((pid=wait_for_pid(-1, &status, WNOHANG))>0) {
#else
    if((pid=wait(&status))>0) {
#endif
#ifdef WIFSIGNALED
        if(WIFSIGNALED(status)) {
            s_log(LOG_INFO, "Local process %d terminated on signal %d",
                pid, WTERMSIG(status));
        } else {
            s_log(LOG_INFO, "Local process %d finished with code %d",
                pid, WEXITSTATUS(status));
        }
#else
        s_log(LOG_INFO, "Local process %d finished with status %d",
            pid, status);
#endif
    }
}
#endif /* !defined USE_WIN32 */

int write_blocking(CLI *c, int fd, u8 *ptr, int len) {
        /* simulate a blocking write */
        /* returns 0 on success, -1 on failure */
    s_poll_set fds;
    int num;

    while(len>0) {
        s_poll_zero(&fds);
        s_poll_add(&fds, fd, 0, 1); /* write */
        switch(s_poll_wait(&fds, c->opt->timeout_busy)) {
        case -1:
            sockerror("write_blocking: s_poll_wait");
            return -1; /* error */
        case 0:
            s_log(LOG_INFO, "write_blocking: s_poll_wait timeout");
            return -1; /* timeout */
        case 1:
            break; /* OK */
        default:
            s_log(LOG_ERR, "write_blocking: s_poll_wait unknown result");
            return -1; /* error */
        }
        num=writesocket(fd, ptr, len);
        switch(num) {
        case -1: /* error */
            sockerror("writesocket (write_blocking)");
            return -1;
        }
        ptr+=num;
        len-=num;
    }
    return 0; /* OK */
}

int read_blocking(CLI *c, int fd, u8 *ptr, int len) {
        /* simulate a blocking read */
        /* returns 0 on success, -1 on failure */
    s_poll_set fds;
    int num;

    while(len>0) {
        s_poll_zero(&fds);
        s_poll_add(&fds, fd, 1, 0); /* read */
        switch(s_poll_wait(&fds, c->opt->timeout_busy)) {
        case -1:
            sockerror("read_blocking: s_poll_wait");
            return -1; /* error */
        case 0:
            s_log(LOG_INFO, "read_blocking: s_poll_wait timeout");
            return -1; /* timeout */
        case 1:
            break; /* OK */
        default:
            s_log(LOG_ERR, "read_blocking: s_poll_wait unknown result");
            return -1; /* error */
        }
        num=readsocket(fd, ptr, len);
        switch(num) {
        case -1: /* error */
            sockerror("readsocket (read_blocking)");
            return -1;
        case 0: /* EOF */
            s_log(LOG_ERR, "Unexpected socket close (read_blocking)");
            return -1;
        }
        ptr+=num;
        len-=num;
    }
    return 0; /* OK */
}

int fdprintf(CLI *c, int fd, const char *format, ...) {
    va_list arglist;
    char line[STRLEN], logline[STRLEN];
    char crlf[]="\r\n";
    int len;

    va_start(arglist, format);
#ifdef HAVE_VSNPRINTF
    len=vsnprintf(line, STRLEN, format, arglist);
#else
    len=vsprintf(line, format, arglist);
#endif
    va_end(arglist);
    len+=2;
    if(len>=STRLEN) {
        s_log(LOG_ERR, "Line too long in fdprintf");
        return -1;
    }
    safecopy(logline, line); /* The line without crlf */
    safeconcat(line, crlf);
    if(write_blocking(c, fd, line, len)<0)
        return -1;
    safestring(logline);
    s_log(LOG_DEBUG, " -> %s", logline);
    return len;
}

int fdscanf(CLI *c, int fd, const char *format, char *buffer) {
    char line[STRLEN], logline[STRLEN], lformat[STRLEN];
    s_poll_set fds;
    int ptr, retval;

    for(ptr=0; ptr<STRLEN-1; ptr++) {
        s_poll_zero(&fds);
        s_poll_add(&fds, fd, 1, 0); /* read */
        switch(s_poll_wait(&fds, c->opt->timeout_busy)) {
        case -1:
            sockerror("fdscanf: s_poll_wait");
            return -1; /* error */
        case 0:
            s_log(LOG_INFO, "fdscanf: s_poll_wait timeout");
            return -1; /* timeout */
        case 1:
            break; /* OK */
        default:
            s_log(LOG_ERR, "fdscanf: s_poll_wait unknown result");
            return -1; /* error */
        }
        switch(readsocket(fd, line+ptr, 1)) {
        case -1: /* error */
            sockerror("readsocket (fdscanf)");
            return -1;
        case 0: /* EOF */
            s_log(LOG_ERR, "Unexpected socket close (fdscanf)");
            return -1;
        }
        if(line[ptr]=='\r')
            continue;
        if(line[ptr]=='\n')
            break;
    }
    line[ptr]='\0';
    safecopy(logline, line);
    safestring(logline);
    s_log(LOG_DEBUG, " <- %s", logline);
    retval=sscanf(line, format, buffer);
    if(retval>=0)
        return retval;
    s_log(LOG_DEBUG, "fdscanf falling back to lowercase");
    safecopy(lformat, format);
    for(ptr=0; lformat[ptr]; ptr++)
        lformat[ptr]=tolower(lformat[ptr]);
    for(ptr=0; line[ptr]; ptr++)
        line[ptr]=tolower(line[ptr]);
    return sscanf(line, lformat, buffer);
}

/* End of network.c */
