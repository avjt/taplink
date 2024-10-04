#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <string>

#include <list>
#include <stack>
#include <map>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/epoll.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/if_tun.h>

using namespace std;

bool	daemonflag = true;
const char
	*logfile = 0, *pidfile = 0;

struct action_t {
	int	fd_in, fd_out;
};

static struct action_t
	__A[2];

unsigned char packet[65536];

// Make the process a daemon
void	daemonize(const char *logfile, const char *pidfile)
{
	pid_t	pid;

	if( (pid = fork()) < 0 ) {
		perror("fork");
		abort();
	} else if (pid > 0) {
		if( pidfile ) {
			FILE	*F;

			F = fopen(pidfile, "w");
			fprintf(F, "%u\n", (unsigned int) pid);
			fclose(F);
		}

		exit(0);
	} else {
		int	fd;

		umask(0);
		if( (fd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 ) {
			perror(logfile);
			abort();
		}

		close(2);
		if( dup2(fd, 2) < 0 ) {
			perror("dup2");
			abort();
		}

		close(1);
		if( dup2(fd, 1) < 0 ) {
			perror("dup2");
			abort();
		}

		close(0);
		close(fd);

		if( setsid() < 0 ) {
			perror("setsid");
			abort();
		}
	}
}

static void usage(const char *s)
{
	cerr << "Usage: " << s << " [options] <tunnel-1> <tunnel-2>\n";
	cerr << "\nOptions:\n";
	cerr << "\t-D               Do not run as a daemon\n"; 
	cerr << "\t-l logfile       Write traces to this file (if daemon)\n"; 
	cerr << "\t-p pidfile       Write PID (if daemon)\n"; 
}

int get_tun(const char *interface)
{
	int fd;
	struct ifreq
		ifr;

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strncpy(ifr.ifr_name, interface, sizeof(ifr.ifr_name));

	if( (fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK)) < 0 ) {
		perror("/dev/net/tun");
		abort();
	}

	if (ioctl(fd, TUNSETIFF, &ifr)) {
		perror(interface);
		abort();
	}

	return fd;
}

int main(int C, char **V)
{
	const char
		*commandname = V[0];
	int	esd, fd[2];
	int	yes = 1, no = 0;
	struct epoll_event
		E;

	// One parameters: users file
	for( ; ; ) {
		int o;

		if( (o = getopt(C, V, "Dl:p:")) == (-1) ) {
			break;
		}

		switch(o) {
		case 'l':
			logfile = optarg;
			break;
		case 'p':
			pidfile = optarg;
			break;
		case 'D':
			daemonflag = false;
			break;
		default:
			cerr << "bad option" << endl;
			usage(commandname);
			return 1;
		}
	}

	C -= (optind - 1);
	V += (optind - 1);

	if( C < 3 ) {
		cerr << "no interfaces" << endl;
		usage(commandname);
		return 1;
	} 

	// Create the EPOLL socket, to which the other sockets will be added
	if( (esd = epoll_create(2)) < 0 ) {
		perror("epoll_create");
		abort();
	}

	// Grab the tunnels
	fd[0] = get_tun( V[1] );
	fd[1] = get_tun( V[2] );

	// Side A
	__A[0].fd_in = fd[0];
	__A[0].fd_out = fd[1];

	memset( &E, 0, sizeof(E) );
	E.events = EPOLLIN;
	E.data.ptr = &__A[0];

	if( (epoll_ctl(esd, EPOLL_CTL_ADD, fd[0], &E)) < 0 ) {
		perror("epoll_ctl");
		abort();
	}

	// Side B
	__A[1].fd_in = fd[1];
	__A[1].fd_out = fd[0];

	memset( &E, 0, sizeof(E) );
	E.events = EPOLLIN;
	E.data.ptr = &__A[1];

	if( (epoll_ctl(esd, EPOLL_CTL_ADD, fd[1], &E)) < 0 ) {
		perror("epoll_ctl");
		abort();
	}

	// Daemonize if necessary
	if( daemonflag ) {
		daemonize(logfile ? logfile : "/dev/null", pidfile);
	}

	// The main loop
	for( ; ; ) {
		int r;

		if( (r = epoll_wait(esd, &E, 1, -1)) < 0 ) {
			perror("epoll_wait");
			abort();
		}

		// Relay
		if( r ) {
			struct action_t *Y;

			Y = (struct action_t *) (E.data.ptr);

			if( E.events & EPOLLIN ) {
				int n;

				if( (n  = read(Y->fd_in, packet, sizeof(packet))) < 0 ) {
					// Ignore for now
					continue;
				}

				if( write(Y->fd_out, packet, n) < 0 ) {
					// Ignore for now
					continue;
				}
			}
		}
	}
}
