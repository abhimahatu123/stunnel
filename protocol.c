/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (c) 1998-2001 Michal Trojnara <Michal.Trojnara@mirt.net>
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
 */

#include "common.h"
#include "prototypes.h"
#include "client.h"

/* protocol-specific function prototypes */
static int smb_client(CLI *);
static int smb_server(CLI *);
static int smtp_client(CLI *);
static int smtp_server(CLI *);
static int pop3_client(CLI *);
static int pop3_server(CLI *);
static int nntp_client(CLI *);
static int nntp_server(CLI *);
static int telnet_client(CLI *);
static int telnet_server(CLI *);
static int RFC2487(int);

int negotiate(char *protocol, int client, CLI * c) {
    if(!protocol)
        return 0; /* No protocol negotiations */
    if(!c)
        return 0; /* No client present */
    log(LOG_DEBUG, "Negotiations for %s(%s side) started", protocol,
        client ? "client" : "server");
    if(!strcmp(protocol, "smb")) {
        if(client)
            return smb_client(c);
        else
            return smb_server(c);
    }
    if(!strcmp(protocol, "smtp")) {
        if(client)
            return smtp_client(c);
        else
            return smtp_server(c);
    }
    if(!strcmp(protocol, "pop3")) {
        if(client)
            return pop3_client(c);
        else
            return pop3_server(c);
    }
    if(!strcmp(protocol, "nntp")) {
        if(client)
            return nntp_client(c);
        else
            return nntp_server(c);
    }
    if(!strcmp(protocol, "telnet")) {
        if(client)
            return telnet_client(c);
        else
            return telnet_server(c);
    }
    log(LOG_ERR, "Protocol %s not supported in %s mode",
        protocol, client ? "client" : "server");
    return -1;
}

static int smb_client(CLI * c) {
    log(LOG_ERR, "Protocol not supported");
    return -1;
}

static int smb_server(CLI * c) {
    log(LOG_ERR, "Protocol not supported");
    return -1;
}

static int smtp_client(CLI * c) {
    char line[STRLEN];
    
    do { /* Copy multiline greeting */
        if(fdscanf(c->remote_fd, "%[^\n]", line)<0)
            return -1;
        if(fdprintf(c->local_wfd, line)<0)
            return -1;
    } while(strncmp(line,"220-",4)==0);

    if(fdprintf(c->remote_fd, "EHLO localhost")<0) /* Send an EHLO command */
        return -1;
    do { /* Skip multiline reply */
        if(fdscanf(c->remote_fd, "%[^\n]", line)<0)
            return -1;
    } while(strncmp(line,"250-",4)==0);
    if(strncmp(line,"250 ",4)!=0) { /* Error */
        log(LOG_ERR, "Remote server is not RFC 1425 compliant");
        return -1;
    }

    if(fdprintf(c->remote_fd, "STARTTLS")<0) /* Send STARTTLS command */
        return -1;
    do { /* Skip multiline reply */
        if(fdscanf(c->remote_fd, "%[^\n]", line)<0)
            return -1;
    } while(strncmp(line,"220-",4)==0);
    if(strncmp(line,"220 ",4)!=0) { /* Error */
        log(LOG_ERR, "Remote server is not RFC 2487 compliant");
        return -1;
    }
    return 0;
}

static int smtp_server(CLI * c) {
    char line[STRLEN];

    if(RFC2487(c->local_rfd)==0)
        return 0; /* Return if RFC 2487 is not used */

    if(fdscanf(c->remote_fd, "220%[^\n]", line)!=1) {
        log(LOG_ERR, "Unknown server welcome");
        return -1;
    }
    if(fdprintf(c->local_wfd, "220%s + stunnel", line)<0)
        return -1;
    if(fdscanf(c->local_rfd, "EHLO %[^\n]", line)!=1) {
        log(LOG_ERR, "Unknown client EHLO");
        return -1;
    }
    if(fdprintf(c->local_wfd, "250-%s Welcome", line)<0)
        return -1;
    if(fdprintf(c->local_wfd, "250 STARTTLS")<0)
        return -1;
    if(fdscanf(c->local_rfd, "STARTTLS", line)<0) {
        log(LOG_ERR, "STARTTLS expected");
        return -1;
    }
    if(fdprintf(c->local_wfd, "220 Go ahead", line)<0)
        return -1;
    return 0;
}

static int pop3_client(CLI * c) {
    char line[STRLEN];

    fdscanf(c->remote_fd, "%[^\n]", line);
    if(strncmp(line,"+OK ",4)) {
        log(LOG_ERR, "Unknown server welcome");
        return -1;
    }
    if(fdprintf(c->local_wfd, line)<0)
        return -1;
    if(fdprintf(c->remote_fd, "STLS")<0)
        return -1;
    fdscanf(c->remote_fd, "%[^\n]", line);
    if(strncmp(line,"+OK ",4)) {
        log(LOG_ERR, "Server does not support TLS");
        return -1;
    }
    return 0;
}

static int pop3_server(CLI * c) {
    log(LOG_ERR, "Protocol not supported in server mode");
    return -1;
}

static int nntp_client(CLI * c) {
    char line[STRLEN];

    fdscanf(c->remote_fd, "%[^\n]", line);
    if(strncmp(line,"200 ",4) && strncmp(line,"201 ",4)) {
        log(LOG_ERR, "Unknown server welcome");
        return -1;
    }
    if(fdprintf(c->local_wfd, line)<0)
        return -1;
    if(fdprintf(c->remote_fd, "STARTTLS")<0)
        return -1;
    fdscanf(c->remote_fd, "%[^\n]", line);
    if(strncmp(line,"382 ",4)) {
        log(LOG_ERR, "Server does not support TLS");
        return -1;
    }
    return 0;
}

static int nntp_server(CLI * c) {
    log(LOG_ERR, "Protocol not supported in server mode");
    return -1;
}

static int telnet_client(CLI * c) {
    log(LOG_ERR, "Protocol not supported");
    return -1;
}

static int telnet_server(CLI * c) {
    log(LOG_ERR, "Protocol not supported");
    return -1;
}

/* 
*
* stunnel can recognize a TLS-RFC2487 connection 
* Use checkConnectionTyp routine from sendmail-tls.c
* If response is true return 1
*
* Pascual Perez       pps@posta.unizar.es 
* Borja Perez         borja@posta.unizar.es 
*
*/

static int RFC2487(int fd) {
    fd_set         fdsRead;
    struct timeval timeout;

    FD_ZERO(&fdsRead);
    FD_SET(fd, &fdsRead);
    memset(&timeout, 0, sizeof(timeout)); /* don't wait */

    switch(select(fd+1, &fdsRead, NULL, NULL, &timeout)) {
    case 0: /* fd not ready to read */
        log(LOG_DEBUG, "RFC 2487 detected");
        return 1;
    case 1: /* fd ready to read */
        log(LOG_DEBUG, "RFC 2487 not detected");
        return 0;
    }
    sockerror("RFC2487 (select)");
    return -1;
}

/* End of protocol.c */
