#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <sys/epoll.h>

#include <linux/if.h>
#include <linux/if_tun.h>

struct counter_t {
	unsigned long long n_bytes, n_packets;
}	up = { 0ULL, 0ULL }, down = { 0ULL, 0ULL };

struct action_t {
	int	fd_r, fd_t;
	struct counter_t
		*counters;
};
	
unsigned int turn = 0;
const char *pattern = "-\\|/";

int tap_alloc(const char *dev)
{
	struct ifreq ifr;
	int fd, err, flags;

	if( (fd = open("/dev/net/tun", O_RDWR)) < 0 ) {
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));

	ifr.ifr_flags = IFF_TAP | IFF_NO_PI ; 
	if( *dev ) {
		strncpy(ifr.ifr_name, dev, IFNAMSIZ);
	}

	if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ){
		close(fd);
		return err;
	}

	if( (flags = fcntl(fd, F_GETFL, 0)) < 0 ) {
		return -1;
	}

	if( fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ) {
		return -1;
	}

	// If the 'dev' had a '%d' we'd return the name with strcpy(dev, ifr.ifr_name) -- but not needed now
	return fd;
}  

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>

void	daemonize( void )
{
	pid_t	pid;

	if( (pid = fork()) < 0 ) {
		perror("fork");
		abort();
	} else if (pid > 0) {
		exit(0);
	} else {
		int	fd;

		umask(0);
		if( (fd = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 ) {
			perror("/dev/null");
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

int	transfer(int r_fd, int t_fd, struct counter_t *p )
{
	static unsigned char
		buffer[1024*16];

	int	r;

	r = read(r_fd, buffer, sizeof(buffer));

	if( r < 0 ) {
		perror("read");
		return -1;
	} else if( r == 0 ) {
		return 0;
	} else {
		r = write(t_fd, buffer, r);
		
		if( r < 0 ) {
			perror("write");
			return r;
		}

		p->n_bytes += r;
		p->n_packets++;

		return r;
	}
}

const char *commandname;
static void usage( void )
{
	fprintf(stderr, "Usage: %s [options]\n", commandname);
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "\t-D               Do not run as a daemon\n"); 
}

int daemonflag = 1;

int main(int C, char **V)
{
	int	fd_u, fd_l, efd;
	struct epoll_event 
		E;
	struct timeval
		now, target, T;
	struct action_t
		__upstream,
		__downstream;

	commandname = V[0];

	for( ; ; ) {
		int o;

		if( (o = getopt(C, V, "D")) == (-1) ) {
			break;
		}

		switch(o) {
		case 'D':
			daemonflag = 0;
			break;
		default:
			usage();
			return 1;
		}
	}

	if ((fd_u = tap_alloc("upper")) < 0 ) {
		perror("upper");
		return 1;
	}

	if ((fd_l = tap_alloc("lower")) < 0 ) {
		perror("lower");
		return 1;
	}

	if( (efd = epoll_create(1)) < 0 ) {
		perror("epoll_create");
		return 1;
	}

	__downstream.fd_r = fd_u;
	__downstream.fd_t = fd_l;
	__downstream.counters = &down;

	memset( &E, 0, sizeof(E) );
	E.events = EPOLLIN;
	E.data.ptr = &__downstream;

	if( (epoll_ctl(efd, EPOLL_CTL_ADD, fd_u, &E)) < 0 ) {
		perror("epoll_ctl:downstream");
		return -1;
	}

	__upstream.fd_r = fd_l;
	__upstream.fd_t = fd_u;
	__upstream.counters = &up;

	memset( &E, 0, sizeof(E) );
	E.events = EPOLLIN;
	E.data.ptr = &__upstream;

	if( (epoll_ctl(efd, EPOLL_CTL_ADD, fd_l, &E)) < 0 ) {
		perror("epoll_ctl:upstream");
		return -1;
	}

	gettimeofday( &now, 0 );
	target.tv_sec = now.tv_sec + 1;
	target.tv_usec = now.tv_usec;

	if( daemonflag ) {
		daemonize();
	}

	for( ; ; ) {
		int r;

		timersub(&target, &now, &T);

		if( (r = epoll_wait(efd, &E, 1, (T.tv_sec * 1000) + (T.tv_usec / 1000))) < 0 ) {
			perror("epoll_wait");
			return -1;
		}

		gettimeofday( &now, 0 );

		if( !timercmp(&now, &target, <) ) {
			if( !daemonflag ) {
				fprintf(stderr, "%c U: %9llu P/s, %12llu B/s, %13llu b/s, D:  %9llu P/s, %12llu B/s, %13llu b/s\r", 
					pattern[turn], 
					up.n_packets, up.n_bytes, 8*up.n_bytes,
					down.n_packets, down.n_bytes, 8*down.n_bytes);
				turn = (turn + 1) % 4;
			}

			up.n_bytes = up.n_packets = down.n_bytes = down.n_packets = 0;
			target.tv_sec = now.tv_sec + 1;
			target.tv_usec = now.tv_usec;
		}

		if( r == 0 ) {
			continue;
		}

		transfer( 
			((struct action_t *)(E.data.ptr))->fd_r, 
			((struct action_t *)(E.data.ptr))->fd_t,
			((struct action_t *)(E.data.ptr))->counters );
	}
}
