/*****************************************************/
/* stunnel.c          version 1.1           97.02.14 */
/* by Michal Trojnara   <mtrojnar@ddc.daewoo.com.pl> */
/* SSLeay support Adam Hernik <adas@infocentrum.com> */
/*             Pawel Krawczyk <kravietz@ceti.com.pl> */
/*****************************************************/

#define STUNNELCERT "/etc/server.pem"
#define STUNNELKEY "/etc/server.pem"
#define BUFFSIZE 8192	/* I/O buffer size */

#include <stdio.h>
#include <unistd.h>	/* for fork, execvp, exit */
#include <errno.h>	/* for errno */
#include <string.h>	/* for strerror */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>	/* for select */
#include <signal.h>	/* for signal */
#include <syslog.h>	/* for openlog, syslog */
#include <ssl.h>
#include <err.h>

/* Correct callback definitions overriding ssl.h */
#define SSL_CTX_set_tmp_rsa_callback(ctx,cb) \
        SSL_CTX_ctrl(ctx,SSL_CTRL_SET_TMP_RSA_CB,0,(char *)cb)
#define SSL_CTX_set_tmp_dh_callback(ctx,dh) \
        SSL_CTX_ctrl(ctx,SSL_CTRL_SET_TMP_DH_CB,0,(char *)dh)

void transfer(SSL *, int);
void make_sockets(int [2]);
static RSA *tmp_rsa_cb(SSL *, int);
static DH *tmp_dh_cb(SSL *, int);
void ioerror(char *);
void sslerror(char *);
void signal_handler(int);

int main(int argc, char* argv[])
{
    int fd[2];
    SSL *ssl;
    SSL_CTX *ctx;

    signal(SIGPIPE, SIG_IGN); /* avoid 'broken pipe' signal */
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGSEGV, signal_handler);
    make_sockets(fd);
    switch(fork()) {
    case -1:	/* error */
        ioerror("fork");
    case  0:	/* child */
        close(fd[0]);
        dup2(fd[1], 0);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[1]);
        execvp(argv[0], argv);
        ioerror("execvp"); /* execvp failed */
    default:	/* parent */
        close(fd[1]);
        openlog("stunnel", LOG_CONS | LOG_NDELAY | LOG_PID, LOG_DAEMON);
        SSL_load_error_strings();
        SSLeay_add_ssl_algorithms();
        ctx=SSL_CTX_new(SSLv23_server_method());
        if(!SSL_CTX_use_RSAPrivateKey_file(ctx, STUNNELKEY, SSL_FILETYPE_PEM))
            sslerror("SSL_CTX_use_RSAPrivateKey_file");
        if(!SSL_CTX_use_certificate_file(ctx, STUNNELCERT, SSL_FILETYPE_PEM))
            sslerror("SSL_CTX_use_certificate_file");
        if(!SSL_CTX_set_tmp_rsa_callback(ctx, tmp_rsa_cb))
            sslerror("SSL_CTX_set_tmp_rsa_callback");
        if(!SSL_CTX_set_tmp_dh_callback(ctx, tmp_dh_cb))
            sslerror("SSL_CTX_set_tmp_dh_callback");
        ssl=SSL_new(ctx);
        SSL_set_fd(ssl, 0);
        if(!SSL_accept(ssl))
            sslerror("SSL_accept");
        transfer(ssl, fd[0]);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
    }
    return 0; /* success */
}

void transfer(SSL *ssl, int tunnel) /* main loop */
{
    fd_set rin, rout;
    int num, fdno, fd_ssl;
    char buffer[BUFFSIZE];

    fd_ssl=SSL_get_fd(ssl);
    FD_ZERO(&rin);
    FD_SET(fd_ssl, &rin);
    FD_SET(tunnel, &rin);
    fdno=(fd_ssl>tunnel ? fd_ssl : tunnel)+1;
    while(1)
    {
        rout=rin;
        if(select(fdno, &rout, NULL, NULL, NULL)<0)
            ioerror("select");
        if(FD_ISSET(fd_ssl, &rout))
        {
            num=SSL_read(ssl, buffer, BUFFSIZE);
            if(num<0)
                sslerror("SSL_read");
            if(num==0)
                return; /* close */
            if(write(tunnel, buffer, num)!=num)
                ioerror("write");
        }
        if(FD_ISSET(tunnel, &rout))
        {
            num=read(tunnel, buffer, BUFFSIZE);
            if(num<0)
                ioerror("read");
            if(num==0)
                return; /* close */
            if(SSL_write(ssl, buffer, num)!=num)
                ioerror("SSL_write");
        }
    }
}

/* Should be done with AF_INET instead of AF_UNIX */
void make_sockets(int fd[2]) /* make pair of connected sockets */
{
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
        ioerror("socketpair");
}

static RSA *tmp_rsa_cb(SSL *s, int export) /* temporary RSA key callback */
{
    static RSA *rsa_tmp = NULL;
 
    if(rsa_tmp == NULL)
    {
        syslog(LOG_DEBUG, "Generating 512 bit RSA key...");
        rsa_tmp=RSA_generate_key(512, RSA_F4, NULL);
        if(rsa_tmp == NULL)
            sslerror("tmp_rsa_cb");
    }
    return(rsa_tmp);
}

static DH *tmp_dh_cb(SSL *s, int export) /* temporary DH key callback */
{
    static DH *dh_tmp = NULL;

    if(dh_tmp == NULL)
    {
        syslog(LOG_DEBUG, "Generating Diffie-Hellman key...");
        if((dh_tmp = DH_new()) == NULL)
            sslerror("DH_new");
        if(!DH_generate_key(dh_tmp))
	    sslerror("DH_generate_key");
        syslog(LOG_DEBUG, "Diffie-Hellman length %d", DH_size(dh_tmp));
    }
    return(dh_tmp);
}

void ioerror(char *fun) /* Input/Output Error handler */
{
    syslog(LOG_ERR, "%s: %s (%d)", fun, strerror(errno), errno);
    exit(1);
}

void sslerror(char *fun) /* SSL Error handler */
{
    char string[120];

    ERR_error_string(ERR_get_error(), string);
    syslog(LOG_ERR, "%s: %s", fun, string);
    exit(2);
}

void signal_handler(int sig) /* Signal handler */
{
    syslog(LOG_ERR, "Received signal %d; terminating.", sig);
    exit(3);
}

