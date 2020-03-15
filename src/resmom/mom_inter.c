/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */
/**
 * @file	mom_inter.c
 */
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <netdb.h>
#if defined(HAVE_SYS_IOCTL_H)
#include <sys/ioctl.h>
#endif
#if !defined(sgi) && !defined(linux)
#include <sys/tty.h>
#endif  /* ! sgi */
#include "portability.h"
#include "pbs_ifl.h"
#include "server_limits.h"
#include "net_connect.h"
#include "libsec.h"
#include "port_forwarding.h"
#include "log.h"


static char cc_array[PBS_TERM_CCA];
static struct winsize wsz;

extern int mom_reader_go;

extern int ptc;
#define XAUTH_LEN   512

/**
 * @brief
 * 	read_net - read data from network till received amount expected
 *
 * @param[in] sock - file descriptor
 * @param[in] buf  - buffer
 * @param[in] amt  - amount of data
 *
 * @return int
 * @retval  >0 amount read
 * @retval  -1 on error
 *
 */
static int
read_net(sock, buf, amt)
int    sock;
char  *buf;
int    amt;
{
	int got;
	int total = 0;

	while (amt > 0) {
		got = CS_read(sock, buf, amt);
		if (got > 0) {	/* read (some) data */
			amt   -= got;
			buf   += got;
			total += got;
		} else if (got == 0)
			break;
		else
			return (-1);
	}
	return (total);
}

/**
 * @brief
 * 	rcvttype - receive the terminal type of the real terminal
 *	Sent over network as "TERM=type_string"
 *
 * @param sock - file descriptor
 *
 * @return string
 *
 */

char *
rcvttype(sock)
int sock;
{
	static char buf[PBS_TERM_BUF_SZ];

	/* read terminal type as sent by qsub */

	if ((read_net(sock, buf, PBS_TERM_BUF_SZ) != PBS_TERM_BUF_SZ) ||
		(strncmp(buf, "TERM=", 5) != 0))
		return NULL;

	/* get the basic control characters from qsub's termial */

	if (read_net(sock, cc_array, PBS_TERM_CCA) != PBS_TERM_CCA) {
		return NULL;
	}

	return (buf);
}

/**
 * @brief
 * 	set_termcc - set the basic modes for the slave terminal, and set the
 *	control characters to those sent by qsub.
 *
 * @param[in] fd - file descriptor
 *
 * @return Void
 *
 */

void
set_termcc(fd)
int fd;
{
	struct termios slvtio;

#ifdef	PUSH_STREAM
	(void)ioctl(fd, I_PUSH, "ptem");
	(void)ioctl(fd, I_PUSH, "ldterm");
#endif	/* PUSH_STREAM */

	if (tcgetattr(fd, &slvtio) < 0)
		return;		/* cannot do it, leave as is */

#ifdef IMAXBEL
	slvtio.c_iflag = (BRKINT|IGNPAR|ICRNL|IXON|IXOFF|IMAXBEL);
#else
	slvtio.c_iflag = (BRKINT|IGNPAR|ICRNL|IXON|IXOFF);
#endif
	slvtio.c_oflag = (OPOST|ONLCR);
#if defined(ECHOKE) && defined(ECHOCTL)
	slvtio.c_lflag = (ISIG|ICANON|ECHO|ECHOE|ECHOK|ECHOKE|ECHOCTL);
#else
	slvtio.c_lflag = (ISIG|ICANON|ECHO|ECHOE|ECHOK);
#endif
	slvtio.c_cc[VEOL]   = '\0';
	slvtio.c_cc[VEOL2]  = '\0';
	slvtio.c_cc[VSTART] = '\021';		/* ^Q */
	slvtio.c_cc[VSTOP]  = '\023';		/* ^S */
#if defined(VDSUSP)
	slvtio.c_cc[VDSUSP] = '\031';		/* ^Y */
#endif
#if defined(VREPRINT)
	slvtio.c_cc[VREPRINT] = '\022';		/* ^R */
#endif
	slvtio.c_cc[VLNEXT] = '\017';		/* ^V */

	slvtio.c_cc[VINTR]  = cc_array[0];
	slvtio.c_cc[VQUIT]  = cc_array[1];
	slvtio.c_cc[VERASE] = cc_array[2];
	slvtio.c_cc[VKILL]  = cc_array[3];
	slvtio.c_cc[VEOF]   = cc_array[4];
	slvtio.c_cc[VSUSP]  = cc_array[5];
	(void)tcsetattr(fd, TCSANOW, &slvtio);
}



/**
 * @brief
 * 	rcvwinsize - receive the window size of the real terminal window
 *
 *	Sent over network as "WINSIZE rn cn xn yn"  where .n is numeric string
 *
 * @param[in] sock - file descriptor
 *
 * @return   error code
 * @retval   0     Success
 * @retval  -1     Failure
 *
 */

int
rcvwinsize(sock)
int sock;
{
	char buf[PBS_TERM_BUF_SZ];

	if (read_net(sock, buf, PBS_TERM_BUF_SZ) != PBS_TERM_BUF_SZ)
		return (-1);
	if (sscanf(buf, "WINSIZE %hu,%hu,%hu,%hu", &wsz.ws_row, &wsz.ws_col,
		&wsz.ws_xpixel, &wsz.ws_ypixel) != 4)
		return (-1);
	return 0;
}

/**
 * @brief
 *	set window size or terminal
 *
 * @param pty - terminal interface
 *
 * @return    error code
 * @retval    0     Success
 * @retval   -1     Failure
 *
 */
int
setwinsize(pty)
int pty;
{
	if (ioctl(pty, TIOCSWINSZ, &wsz) < 0) {
		perror("ioctl TIOCSWINSZ");
		return (-1);
	}
	return (0);
}


/**
 * @brief
 *	reader process - reads from the remote socket, and writes
 *	to the master pty
 *
 * @param[in] s - filr descriptor
 * @param[in] ptc - master file descriptor
 * @param[in] command - shell command
 *
 */
int
mom_reader(s, ptc, command)
int s;
int ptc;
char *command; /* shell command(s) to be sent to the PTY before user data from the socket */
{
	char buf[PF_BUF_SIZE];
	int c;

	/* send shell command(s) to the PTY input stream */
	c = command ? strlen(command) : 0;
	if (c > 0) {
		int wc;
		char *p = command;

		while (c) {
			if ((wc = write(ptc, p, c)) < 0) {
				if (errno == EINTR) {
					continue;
				}
				return (-1);
			}
			c -= wc;
			p += wc;
		}
	}

	/* read from the socket, and write to ptc */
	while (mom_reader_go) {
		c = CS_read(s, buf, sizeof(buf));
		if (c > 0) {
			int wc;
			char *p = buf;

			while (c) {
				if ((wc = write(ptc, p, c)) < 0) {
					if (errno == EINTR) {
						continue;
					}
					return (-1);
				}
				c -= wc;
				p += wc;
			}
		} else if (c == 0) {
			return (0);
		} else if (c < 0) {
			if (errno == EINTR)
				continue;
			else {
				return (-1);
			}
		}
	}
	return 0;
}

/**
 * @brief
 *      This functions sets the job directory to PBS_JOBDIR for private sandbox
 *
 * @param command[in] - this parameter contains the job directory in case of
 *                      private sandbox.
 * @return int
 * @retval 0 Success
 * @retval -1 Failure
 *
 */
int
setcurrentworkdir(char *command)

{
	int c;

	/* send shell command(s) to the PTY input stream */
	c = command ? strlen(command) : 0;
	if (c > 0) {
		int wc;
		char *p = command;

		while (c) {
			if ((wc = write(ptc, p, c)) < 0) {
				if (errno == EINTR) {
					continue;
				}
				return (-1);
			}
			c -= wc;
			p += wc;
		}
	}
	return 0;
}

/**
 * @brief
 *        This function reads the data from remote socket and write it to pty.
 *
 * @param[in] s - socket fd from where data needs to be read.
 *
 * @return  int
 * @retval    0  Success
 * @retval   -1  Failure
 * @retval   -2  Peer Closed
 *
 */
int
mom_reader_Xjob(int s)
{
	static char buf[PF_BUF_SIZE];
	int c;
	/* read from the socket, and write to ptc */
	c = CS_read(s, buf, sizeof(buf));
	if (c > 0) {
		int wc;
		char *p = buf;

		while (c) {
			if ((wc = write(ptc, p, c)) < 0) {
				if (errno == EINTR) {
					continue;
				}
				return (-1);
			}
			c -= wc;
			p += wc;
		}
	} else if (c == 0) {
		/* If control reaches here, then it means peer has closed the
		 * connection
		 */
		return (-2);
	} else if (c < 0) {
		if (errno == EINTR) {
			return (0);
		}
		else {
			return (-1);
		}
	}
	return (0);
}


/**
 * @brief
 * 	Writer process: reads from master pty, and writes
 * 	data out to the rem socket
 *
 * @param[in] s - socket fd
 * @param[in] ptc - master file descriptor
 *
 * @return    error code
 * @retval    0     Success
 * @retval   -1     Failure
 *
 */
int
mom_writer(s, ptc)
int s;
int ptc;
{
	char buf[PF_BUF_SIZE];
	int c;

	/* read from ptc, and write to the socket */
	while (1) {
		c = read(ptc, buf, sizeof(buf));
		if (c > 0) {
			int wc;
			char *p = buf;

			while (c) {
				if ((wc = CS_write(s, p, c)) < 0) {
					if (errno == EINTR) {
						continue;
					}
					return (-1);
				}
				c -= wc;
				p += wc;
			}
		} else if (c == 0) {
			return (0);
		} else if (c < 0) {
			if (errno == EINTR)
				continue;
			else {
				return (-1);
			}
		}
	}
}

/**
 * @brief
 *      connect to the qsub that submitted this interactive job
 *
 * @param hostname[in] - hostname of the submission host where qsub is running.
 * @param port[in] - port number on which qsub is accepting connection.
 *
 * @return int
 * @retval >=0 the socket obtained
 * @retval  -1 PBS_NET_RC_FATAL
 * @retval  -2 PBS_NET_RC_RETRY
 *
 */
int
conn_qsub(char *hostname, long port)
{
	pbs_net_t hostaddr;

	if ((hostaddr = get_hostaddr(hostname)) == (pbs_net_t)0)
		return (PBS_NET_RC_FATAL);

	/* Yes, the qsub is listening, but for authentication
	 * purposes mom wants authenticate as a server - not as
	 * a client
	 */

	return (client_to_svr(hostaddr, (unsigned int)port, B_SVR));
}

/**
 * @brief       This function creates a socket for listening for X11
 *              connections.The socket is created only for jobs that
 *              require X forwarding .
 *
 * @param socks[in/out] - Socks structure which keeps track of
 *                        sockets that are active and data read/written by
 *                        peers.
 * @param x11_use_localhost[in] - Non-zero value to use localhost only.
 * @param display[out] - sets the display number and screen number.
 * @param homedir[in] - uses this home directory to put in the environment.
 * @param x11authstr[in] - used to get the X11 protocol, hex data and screen.
 *
 * @return int - Return a suitable display number (>0) for the DISPLAY
 *               variable
 * @retval -1 Failure
 *
 */
int
init_x11_display(
	struct pfwdsock *socks,
	int x11_use_localhost,
	char *display,
	char *homedir,
	char *x11authstr)
{
	int display_number, sock, i;
	u_short port;
	struct addrinfo hints, *ai, *aitop;
	char strport[NI_MAXSERV];
	int gaierr, n, num_socks = 0, ret = 0;
	unsigned int x11screen;
	char x11proto[XAUTH_LEN], x11data[XAUTH_LEN];
	char format[XAUTH_LEN];
	char *homeenv;
	char logit[512] = {0};
	char func[] = "init_x11_display";

	*display = '\0';
	if ((homeenv = malloc(sizeof("HOME=") + strlen(homedir) + 1)) == NULL) {
		/* FAILURE - cannot alloc memory */
		sprintf(logit, "Malloc Failure : %.100s\n",
			strerror(errno));
		log_err(errno, func, logit);

		return (-1);
	}

	setenv("HOME", homedir, 1);

	for (n = 0; n < NUM_SOCKS; n++)
		socks[n].active = 0;

	x11proto[0] = x11data[0] = '\0';
	format[0] = '\0';

	sprintf(format, " %%%d[^:]: %%%d[^:]: %%u", XAUTH_LEN-1, XAUTH_LEN-1);

	errno = 0;

	if ((n = sscanf(x11authstr, format,
		x11proto,
		x11data,
		&x11screen)) != 3) {
		sprintf(logit, "sscanf(%s)=%d failed: %s\n",
			x11authstr,
			n,
			strerror(errno));
		log_err(errno, func, logit);
		free(socks);
		return (-1);
	}

	for (display_number = X11OFFSET; display_number < MAX_DISPLAYS;
		display_number++) {
		port =  X_PORT + display_number;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_flags = x11_use_localhost ? 0 : AI_PASSIVE;
		hints.ai_socktype = SOCK_STREAM;
		ret = snprintf(strport, sizeof(strport), "%d", port);
		if (ret >= sizeof(strport)) {
			log_err(-1, func , "strport overflow");
		}


		if ((gaierr = getaddrinfo(NULL, strport, &hints, &aitop)) != 0) {
			sprintf(logit, "getaddrinfo: %.100s\n",
				gai_strerror(gaierr));
			log_err(errno, func, logit);
			free(socks);
			return (-1);
		}

		/* create a socket and bind it to display_number */
		for (ai = aitop; ai != NULL; ai = ai->ai_next) {
			if (ai->ai_family != AF_INET)
				continue;
			sock = socket(ai->ai_family, SOCK_STREAM, 0);
			if (sock < 0) {
				if ((errno != EINVAL) && (errno != EAFNOSUPPORT)) {
					sprintf(logit, "socket: %.100s\n",
						strerror(errno));
					log_err(errno, func, logit);
					free(socks);
					return (-1);
				} else{
					sprintf(logit, "Socket family %d *NOT* supported\n",
						ai->ai_family);
					log_err(errno, func, logit);
					continue;
				}
			}
			i = 1;
			setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&i, sizeof(i));
			if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
				log_err(errno, func, strerror(errno));
				close(sock);
				if (ai->ai_next)
					continue;
				for (n = 0; n < num_socks; n++) {
					close(socks[n].sock);
				}
				num_socks = 0;
				break;
			}

			socks[num_socks].sock = sock;
			socks[num_socks].active = 1;
			num_socks++;
			if (x11_use_localhost) {
				if (num_socks == NUM_SOCKS)
					break;
			} else{
				break;
			}
		}
		freeaddrinfo(aitop);
		if (num_socks > 0)
			break;
	} /* END for (display) */
	if (display_number >= MAX_DISPLAYS) {
		sprintf(logit, "Failed to allocate internet-domain X11 display socket.\n");
		log_err(errno, func, logit);
		free(socks);
		return (-1);
	}

	/* Start listening for connections on the socket. */
	for (n = 0; n < num_socks; n++) {
		if (listen(socks[n].sock, 10) < 0) {
			sprintf(logit, "listen : %.100s\n",
				strerror(errno));
			log_err(errno, func, logit);
			close(socks[n].sock);
			free(socks);
			return (-1);
		}
		socks[n].listening = 1;
	} /* END for (n) */

	/* setup local xauth */

	sprintf(display, "localhost:%u.%u",
		display_number,
		x11screen);

	return (display_number);
}
