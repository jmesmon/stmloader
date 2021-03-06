#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <getopt.h>

int debug = 0;
#define unlikely(x)     __builtin_expect((x),0)

#define INFO(...) do {                                                     \
		if (unlikely(debug)) {                                     \
			fprintf(stderr,"INFO: ");                          \
			perror_at_line(0,0,__func__,__LINE__,__VA_ARGS__); \
		}                                                          \
	} while(0)

#define WARN(_exitnum_,_errnum_,...) do {               \
		fprintf(stderr,"WARN: ");               \
		perror_at_line(_exitnum_,_errnum_,      \
			__func__,__LINE__,__VA_ARGS__); \
	} while(0)

/* ST bootloader link */
struct stblink {
	int fd;
	long utimeout;
};


void __attribute__((format(printf,5,6)))
perror_at_line(int status, int errnum, const char *fname,
	unsigned int linenum, const char *format, ...)
{
	va_list vl;
	va_start(vl,format);

	fflush(stdout);
	fprintf(stderr,"%s:%d ",fname,linenum);
	if (errnum)
		fprintf(stderr,"[%s] : ",strerror(errnum));
	else
		fprintf(stderr," : ");
	vfprintf(stderr,format,vl);
	fputc('\n',stderr);
	fflush(stderr);
	if (status)
		exit(status);
}

enum to_boot {
	c_get      = 0x00,
	c_getv     = 0x01,
	c_get_id   = 0x02,
	c_read     = 0x11,
	c_go       = 0x21,
	c_write    = 0x31,
	c_erase    = 0x43,
	c_w_prot   = 0x63,
	c_w_unprot = 0x73,
	c_r_prot   = 0x82,
	c_r_unprot = 0x92,

	i_start    = 0x7F
};

enum from_boot {
	b_ack   = 0x79,
	b_nack  = 0x1F
};

/*
 * usart even parity
 * 115200 baud
 */

/*
 * checksum: all received bytes are XORed. A byte containing
 * the computed XOR of all previous bytes is added to the
 * end of each communication (checksum byte). By XORing all
 * received bytes, data + checksum, the result at the end of
 * the packet must be 0x00.
 */

enum returns {
	kERR = -3,
	kUNEX = -2,
	kTIME = -1,
	kACK = 0,
	kNACK = 1
};

// -3 = error
int s_read(struct stblink *stb, void *buf, size_t nbyte) {
	size_t pos = 0;
	ssize_t ret;
	size_t sret;
	fd_set fds;
	struct timeval timeout = { .tv_sec=0, .tv_usec=0 };

	do {
		FD_ZERO(&fds);
		FD_SET(stb->fd, &fds);
		timeout.tv_sec  = 0;
		timeout.tv_usec = stb->utimeout;

		sret = select(stb->fd + 1, &fds, NULL, NULL, &timeout);
		if (sret == 0) return kTIME;

		if (sret != 1) {
			WARN(0, errno, "select");
			return kERR;
		}
	
		ret = read(stb->fd, buf + pos, nbyte - pos);
		if (ret == -1) {
			WARN(0,errno,"read failed");
			return kERR;
		}
		pos += ret;
	} while (pos < nbyte);
	return 0;
}

// 1 = nack, 0 = ack, -1 = timeout, -3 = error
int wait_ack(const struct stblink *stb)
{
	char tmp;
	int ret;
	ssize_t rret;
	fd_set fds;
	do {
		struct timeval timeout = { .tv_sec=0, .tv_usec=stb->utimeout };
		FD_ZERO(&fds);
		FD_SET(stb->fd, &fds);

		ret = select(stb->fd + 1, &fds, NULL, NULL, &timeout);

		switch (ret) {
		case  0: 
			INFO("timeout");
			return kTIME;

		case  1: 
			rret = read(stb->fd, &tmp, 1);
			if (rret == 1) {
				if (tmp == b_ack) {
				INFO("got ack");
					return 0;
				}
				if (tmp == b_nack) {
					INFO("got nack");
					return 1;
				}
				WARN(0, 0, "recieved junk byte %x", tmp);
			} else {
				WARN(0, 0, "read %zi\n", rret);
			}
			break;
		default:
		case -1:
			WARN(0, errno, "select error");
			return kERR;
		}
	} while (1);
}

// 0 = success, 1 = nacked, -1 = timeout, -2 = unknowndata, -3 = error
int bootloader_init(struct stblink *stb)
{
	char tmp = i_start;
	int ret;
	do {
		do {
			ret = write(stb->fd, &tmp, 1);
		} while (ret == 0);
		if (ret == -1) return kERR - 1;
		ret = wait_ack(stb);
	} while(ret < 0);
	return ret;
}

int send_command(struct stblink *stb, enum to_boot com)
{
	char tmp[2];
	ssize_t ret;
	size_t pos = 0;
	INFO("sending 0x%02X",com);
	tmp[0] = com;
	tmp[1] = ~com;
	do {
		ret = write(stb->fd, tmp + pos, sizeof(tmp) - pos);
		if (ret == -1)
			return kERR - 1;
		pos += ret;
	} while(pos < sizeof(tmp));
	
	return wait_ack(stb);
}

int send_command2(struct stblink *stb, enum to_boot com)
{
	char tmp[2];
	ssize_t wret;
	size_t pos = 0;
	int aret;
	INFO("sending 0x%02X",com);
	tmp[0] = com;
	tmp[1] = ~com;
	do {
		do {
			wret = write(stb->fd, tmp + pos, sizeof(tmp) - pos);
			if (wret == -1) {
				WARN(0, errno, "send command");
				return kERR - 1;
			}
			pos += wret;
		} while(pos < sizeof(tmp));
		
		aret = wait_ack(stb);
	
	} while(aret == kTIME);
	INFO("sent command (%d)",aret);
	return aret;	
}

int serial_init(int fd) {
	struct termios tp_o;
	struct termios tp_n;
	int ret = tcgetattr(fd, &tp_o);
	if (ret == -1) {
		WARN(0,errno,"tcgetattr");
		return 3;
	}

	tp_n = tp_o;

	tp_n.c_iflag = INPCK | IXON | IXOFF;
	tp_n.c_oflag = 0;
	tp_n.c_cflag = CS8 | CREAD | PARENB | CLOCAL;
	tp_n.c_lflag = 0;

	ret = cfsetispeed(&tp_n,B115200);
	if (ret == -1) {
		WARN(0,errno,"cfsetispeed");
		return 4;
	}
	ret = cfsetospeed(&tp_n,B115200);
	if (ret == -1) {
		WARN(0,errno,"cfsetospeed");
		return 5;
	}

	ret = tcsetattr(fd, TCSAFLUSH, &tp_n);
	if (ret == -1) {
		WARN(0,errno,"tcsetattr");
		return 6;
	}

	return 0;
}

uint8_t gen_check(const void *data, uint32_t len)
{
	const uint8_t *cdata = data;
	const uint8_t *pos;
	uint8_t check = 0; 
	for(pos = cdata; pos < (cdata + len); pos++) {
		check ^= *pos;
	}
	return check;
}

int send_data_check(struct stblink *stb, const void *data, uint32_t len)
{
	uint8_t check = gen_check(data, len);
	write(stb->fd, data, len);
	return write(stb->fd, &check, 1);
}

int get_id(struct stblink *stb) {
	int ret;
	do {
		ret = send_command(stb, c_get_id);
	} while (ret == kTIME);
	INFO("send command c_geti (%d)", ret);
	if (ret) {
		WARN(0, errno, "send command");
		return ret -1;
	}

	char len;
	ret = s_read(stb, &len, 1);

	char *data = malloc(len+1);
	ret = s_read(stb, data, len + 1);

	wait_ack(stb);

	printf("GET_ID\n"
	       " PID: %02x " 
	      ,data[0]);
	int i;
	for (i = 0; i <	len; i++) 
		printf("%02x ",data[i+1]);
	putchar('\n');
	return 0;
}


int get_version(struct stblink *stb) {
	INFO("getting version\n");
	int ret;
	ret = send_command(stb, c_getv);
	INFO("send command c_getv (%d)",ret);
	if (ret) {
		WARN(0,errno,"send command");
		return ret -1;
	}

	char data[3];
	ret = s_read(stb, data, 3);
	if (ret) {
		WARN(0,errno,"s_read of 3 bytes returned %d",ret);
		return -4;
	}
		
	ret = wait_ack(stb);
	if (ret) {
		WARN(0,errno,"wait_ack returned %d",ret);
	}

	printf("GETV\n"
	       " bootloader version: %x\n"
	       " option byte 1 (0) : %x\n"
	       " option byte 2 (0) : %x\n"
	      ,data[0],data[1],data[2]);
	return 0;
}

int get_commands(struct stblink *stb) {
	int ret;
	do {
		ret = send_command(stb, c_get);
	} while (ret == kTIME);
	INFO("sent command c_get (%d)",ret);
	if (ret) {
		WARN(0,errno,"send_command returned %d",ret);
		return ret - 1;
	}

	uint8_t n;
	ret = s_read(stb, &n, 1);
	INFO("s_read of numbytes returned %d and got %u",ret,n);

	if (ret)  {
		WARN(0,errno,
			"s_read of numbytes returned %d and got %u",ret,n);
		return ret - 2;
	}

	uint8_t *get_d = malloc(n);
	if (!get_d) {
		perror("malloc");
		return -7;
	}
	
	ret = s_read(stb, get_d, n);
	if (ret) {
		WARN(0,errno,
			"s_read of commands returned %d",ret);
		return ret -3;
	}
	INFO("s_read{c_get data}: %d",ret);

	ret = wait_ack(stb);
	if (ret) {
		WARN(0,errno,"wait_ack: %d",ret);
	}
	size_t i;
	printf("GET\n"
	       " bootloader version: %x\n"
	       " supported commands: ",get_d[0]);
	for (i = 1; i < n; i++) {
		printf("%x, ",get_d[i]);
	}
	putchar('\n');
	return 0;
}

int cmd_erase_mem(struct stblink *stb, uint32_t addr, uint8_t len )
{
	/*
	 * Notes:
	 *  writes must be work aligned.
	 *  start address is not checked very closely by bootloader.
	 *  or at all.
	 */
	// send command + invert. 1,2
	// wait for ack.
	int ret = send_command2(stb, c_write);
	if (ret) {
		WARN(0,errno,"send_command returned %d",ret);
		return ret - 1;
	}
	
	// Send N.
	// 1. 0 < N < 254 == erase N + 1 pages
	// 2. N == 255 -> erase all pages
	
	// 1:
	// Send N
	// Send Page Numbers
	// Send XOR of N & all Page Numbers
	
	// 2:
	// Send N
	// Send inverse of N
	
	// 1,2:
	
	// wait ack
	
	
	return 0;
}

int cmd_write_mem(struct stblink *stb, uint32_t addr, void *data, size_t len)
{
	/*
	 * Notes:
	 *  writes must be work aligned.
	 *  start address is not checked very closely by bootloader.
	 *  or at all.
	 */
	// send command + invert. 1,2
	// wait for ack.
	int ret = send_command2(stb, c_write);
	if (ret) {
		WARN(0,errno,"send_command returned %d",ret);
		return ret - 1;
	}
	
	// Send addr and checksum (XOR of all components)
	
	// wait ack
	
	// send num bytes to be written - 1

	// send all data
	
	// send checksum (XOR of numbytes and all data)
	
	// wait ack
	
	return 0;
}

int cmd_go(struct stblink *stb, uint32_t addr) 
{
	// send command + invert. 1,2
	// wait for ack	 (note: fails if ROP enabled)
	int ret = send_command2(stb, c_go);
	if (ret) {
		WARN(0,errno,"send_command returned %d",ret);
		return ret - 1;
	}

	// send address (3,4,5,6)
	
	// send xor checksum.
	
	// wait for ack.

	return 0;
}

int cmd_read_mem(struct stblink *stb, uint32_t addr) 
{
	// send command + invert. 1,2
	// wait for ack	
	int ret = send_command2(stb, c_read);
	if (ret) {
		WARN(0,errno,"send_command returned %d",ret);
		return ret - 1;
	}

	// Send 4 byte address. MSB first. 3,4,5,6
	
	// Send Checksum (XOR of address bytes) 7
		
	// Wait for ack.
	
	// Send ( [number of bytes to read] - 1 ) 8
	
	// Send Checksum (XOR byte 8 (with 0xFF), compliment) 9
	
	// Wait for ack.
	
	// Read data.
	
	return 0;
}

#define MSK(_msk_,_x_) !!((_msk_)&(_x_))

int tty_ctrl(int fd, int pin_msk, bool high) {
	int s;
	int r = ioctl(fd, TIOCMGET, &s);
	if (r)
		WARN(0,errno,"ioctl_TIOCMGET returned %d : \"%s\"",r,strerror(r));
	
	if (pin_msk) {
		int sb = s;
		if (high) {
			sb |= pin_msk;
		} else {
			sb &= pin_msk;
		}
		
		r = ioctl(fd, TIOCMSET, &sb);
		if (r)
			WARN(0,errno,"ioctl_TIOCMSET returned %d : \"%s\"",r,strerror(r));
	}
	return s;
}

void tty_printctrl(int fd) {
	int status = tty_ctrl(fd,0,0);
	
	printf("0x%02x :: ",status);
	printf("CAR:%d ",MSK(TIOCM_CAR,status));
	printf("RNG:%d ",MSK(TIOCM_RNG,status));
	printf("DSR:%d ",MSK(TIOCM_DSR,status));
	printf("DTR:%d ",MSK(TIOCM_DTR,status));
	printf("RTS:%d ",MSK(TIOCM_RTS,status));
	printf("CTS:%d ",MSK(TIOCM_CTS,status));
	#if defined(TIOCM_ST)
	printf("ST:%d " ,MSK(TIOCM_ST,status));
	#endif
	#if defined(TIOCM_SR)
	printf("SR:%d ",MSK(TIOCM_SR,status));
	#endif
	putchar('\n');
}

const char optstr[] = "hDs:t:iIcvprgweXxZzT";

void usage(char *name) {
	fprintf(stderr,
		"usage: %s [options] [actions]\n"
		"options: -h            help (show this)\n"
		"         -D            debugging output\n"
		"         -t useconds   change serial timeout\n"
		"         -s <tty>      serial port\n"
		"actions: -i            initialize bootloader\n"
		"         -I            do IFI reset (play with RTS/DTR)\n"
		"         -c            get boot supported commands\n"
		"         -v            get boot version\n"
		"         -p            get pid\n"
		"         -r            read memory\n"
		"         -g            \"go\", execute\n"
		"         -w            write memory\n"
		"         -e            erase memory\n"
		"         -X sector:ct  write protect\n"
		"         -x            write unprotect\n"
		"         -Z            readout protect\n"
		"         -z            readout unprotect\n"
		"         -T            just read ctrl lines\n"
		"\n"
		"example : \n"
		"> ./stmboot -s /dev/ttyUSB0 -i -c\n\n"
		       ,name);
}

void cmd_mem_w_protect(void)
{

}

void cmd_mem_r_protect(void)
{

}

int main(int argc, char **argv) {

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	char *serial_s = "";

	struct stblink stb = { -1, 200000 };

	int opt;
	while ( (opt = getopt(argc,argv,optstr)) != -1 ) {
		switch(opt) {
		case '?':
			WARN(-1,0,"bad option %c",optopt);
			break;

		case 'h':
			usage(argv[0]);
			return 1;
			
		default:
			WARN(-1,0,"unimplimented option '%c'",opt);
			break;

		case 's': {
			if (stb.fd >= 0) {
				INFO("closing already open serial \"%s\".",
					serial_s);
				close(stb.fd);
			}
			serial_s = optarg;
			INFO("opening serial port \"%s\".",serial_s);
			stb.fd = open(serial_s, O_RDWR);
			if (stb.fd < 0) {
				WARN(-2,errno,
					"opening serial port \"%s\" failed",
					serial_s);
			}
			int ret = serial_init(stb.fd);
			if (ret) {
				WARN(-1,errno,
					"could not initialize serial \"%s\", %x",
					serial_s,ret);
			}
			break;
		}

		case 'T':
			do {
				tty_printctrl(stb.fd);
				usleep(stb.utimeout);
			} while(1);
			return 0;

		case 'D':
			debug = 1;
			INFO("debuging enabled");
			break;
		case 't': { 
			long tmp;
			int ret = sscanf(optarg,"%li",&tmp);
			if (ret != 1) {
				WARN(-2,errno,
					"specified timeout (\"%s\") invalid",
					optarg); 
			}
			stb.utimeout = tmp;
			INFO("timeout changed to %li usecs", stb.utimeout);
			break;
		}

		case 'i': {
			INFO("connecting to bootloader....");
			int ret;
			do {
				ret = bootloader_init(&stb);
				if (ret <= kERR) {
					WARN(ret,errno,"bootloader_init: %d",ret);
				}
			} while (ret < 0);
			INFO("connected to bootloader : %d",ret);
			break;	
		}

		case 'c':
			get_commands(&stb);
			break;
		case 'v':
			get_version(&stb);
			break;
		case 'p':
			get_id(&stb);
			break;
		}
	}

	INFO("optind %d, argc %d",optind, argc);

	if ( (argc-optind) > 0) {
		WARN(0,0,"unrecognized parameters (%d of them).",argc-optind);
	}

	return 0;
}
