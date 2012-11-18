#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <math.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <netdb.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <pthread.h>

#include "celt/celt.h"
#include "celt/celt_types.h"

#include "types.h"
#include "ao.h"


#define		PORT		9930

#define		BUFSIZE		18UL	// 2 ^ N

#define		SYSFREQ		1E6	// 1000000 per sec
#define		CLIENTMAGIC		0xE1BE5E14
#define		SERVERMAGIC		0xE1BE5E54
#define		FRAMESIZE	24

#define		PACKET_INVALID	0
#define		PACKET_RESTART	1
#define		PACKET_PROCEED	2

#define		report_exceptional_condition() abort ()

pthread_mutex_t mdata = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t bcdata = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t leddata = PTHREAD_MUTEX_INITIALIZER;

struct ring_buffer {
	void *		address;

	unsigned long	count_bytes;
	unsigned long	write_offset_bytes;
	unsigned long	read_offset_bytes;
	unsigned long	elem_count;
	unsigned long	max_size;
};


struct thread_data {
	struct ring_buffer *	rbuf;
	struct AOutput *	device;
	int			sample_buffer_size;
	int			is_debug;
};

struct bthread_data {
	uint16_t		uuid;
	uint16_t		session_id;
	uint32_t		start;
	uint16_t	*	jitter;
	uint16_t	*	mean;
	uint64_t	*	frames;
};

enum recv_state {
	RECV_READY = 1,
	RECV_PLAYING_STREAM
};

struct ledthread_data {
	enum recv_state * state;
};

//Warning order should be at least 12 for Linux
void ring_buffer_create(struct ring_buffer * buffer, unsigned long order) {
	char path[] = "/dev/shm/ring-buffer-XXXXXX";
	int file_descriptor;
	void * address;
	int status;

	if ((file_descriptor = mkstemp(path)) < 0)
		report_exceptional_condition ();

	if (status = unlink(path))
		report_exceptional_condition ();

	buffer->count_bytes = 1UL << order;
	buffer->write_offset_bytes = 0;
	buffer->read_offset_bytes = 0;

	if (status = ftruncate(file_descriptor, buffer->count_bytes))
		report_exceptional_condition();

	buffer->address = mmap(NULL, buffer->count_bytes << 1, PROT_NONE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	if (buffer->address == MAP_FAILED)
		report_exceptional_condition();

	address =
		mmap(buffer->address, buffer->count_bytes, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_SHARED, file_descriptor, 0);

	if (address != buffer->address)
		report_exceptional_condition();

	address = mmap(buffer->address + buffer->count_bytes,
			buffer->count_bytes, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_SHARED, file_descriptor, 0);

	if (address != buffer->address + buffer->count_bytes)
		report_exceptional_condition();

	if (status = close(file_descriptor))
		report_exceptional_condition();

	buffer->elem_count = 0;
	buffer->max_size = buffer->count_bytes >> 12UL;
}

void ring_buffer_free(struct ring_buffer * buffer) {
	int status;

	status = munmap(buffer->address, buffer->count_bytes << 1);
	if (status)
		report_exceptional_condition();
}

void * ring_buffer_write_address (struct ring_buffer * buffer) {
  /*** void pointer arithmetic is a constraint violation. ***/
	return buffer->address + buffer->write_offset_bytes;
}

void ring_buffer_write_advance(struct ring_buffer * buffer,
				unsigned long count_bytes) {
	buffer->write_offset_bytes += count_bytes;
	buffer->elem_count++;
}

void * ring_buffer_read_address(struct ring_buffer * buffer) {
	if (buffer->elem_count > 0)
		return buffer->address + buffer->read_offset_bytes;
	else
		return NULL;
}

void ring_buffer_read_advance(struct ring_buffer * buffer,
				unsigned long count_bytes) {
	buffer->read_offset_bytes += count_bytes;
	buffer->elem_count--;

	if (buffer->read_offset_bytes >= buffer->count_bytes) {
		buffer->read_offset_bytes -= buffer->count_bytes;
		buffer->write_offset_bytes -= buffer->count_bytes;
	}
}

unsigned long ring_buffer_count_bytes(struct ring_buffer * buffer) {
	return buffer->write_offset_bytes - buffer->read_offset_bytes;
}

unsigned long ring_buffer_count_free_bytes(struct ring_buffer * buffer) {
	return buffer->count_bytes - ring_buffer_count_bytes(buffer);
}

unsigned long ring_buffer_count_length(struct ring_buffer * buffer) {
	return buffer->elem_count;
}

void ring_buffer_clear(struct ring_buffer * buffer) {
	buffer->write_offset_bytes = 0;
	buffer->read_offset_bytes = 0;
}

void diep(char * s) {
	perror(s);
	exit(1);
}

uint64_t getcount(void) {
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec * SYSFREQ + t.tv_usec;
}

static void * thread_func(void * vptr_args) {
	struct thread_data * tdata = vptr_args;
	int * tmpbuf = (int *) malloc(tdata->sample_buffer_size * sizeof(char));
	int has_data;
	
	printf("Playing thread has started\n");
	
	while (1) {
		has_data = 0;

		pthread_mutex_lock(&mdata);
		if (ring_buffer_read_address(tdata->rbuf) != NULL) {
			memcpy(tmpbuf, ring_buffer_read_address(tdata->rbuf),
				tdata->sample_buffer_size);
			ring_buffer_read_advance(tdata->rbuf,
				tdata->sample_buffer_size);
			has_data++;
		}
		pthread_mutex_unlock(&mdata);
		if (has_data)
			aout_play(tdata->device, tmpbuf, tdata->sample_buffer_size);
		else
			usleep(10);
	}
	return NULL;
}

static void * bcast_thread_func(void * vptr_args) {
	struct ifaddrs * ifaddr, * ifa;
	int s, on = 1, i, slen = sizeof(struct sockaddr_in), sock;
	struct sockaddr_in si_other;
	struct bthread_data * pbtdata = (struct bthread_data *)vptr_args;
	char   data[FRAMESIZE], * pdata = (char *)&data;

	printf("Broadcasting thread has started\n");

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
	memset((char *) &si_other, 0, sizeof(si_other));
	si_other.sin_family = AF_INET;
	si_other.sin_port = htons(PORT + 1);

	for (;;) {
		if (getifaddrs(&ifaddr) != -1) {
			for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
				if (ifa->ifa_addr == NULL)
					continue;

				if (ifa->ifa_addr->sa_family == AF_INET) {
					memset(pdata, 0, FRAMESIZE);
					*((uint32_t *)(pdata + 0)) = CLIENTMAGIC;
					*((uint16_t *)(pdata + 4)) = pbtdata->uuid;
					*((uint16_t *)(pdata + 6)) = pbtdata->session_id;
					*((uint32_t *)(pdata + 8)) = time(NULL) - pbtdata->start;

					pthread_mutex_lock(&bcdata);
					*((uint16_t *)(pdata + 12)) = *(pbtdata->jitter);
					*((uint16_t *)(pdata + 14)) = *(pbtdata->mean);
					*((uint64_t *)(pdata + 16)) = *(pbtdata->frames);
					pthread_mutex_unlock(&bcdata);
					
					si_other.sin_addr = ((struct sockaddr_in *)(ifa->ifa_ifu.ifu_broadaddr))->sin_addr;
					sendto(sock, pdata, FRAMESIZE, 0, (struct sockaddr *)&si_other, slen);
				}
			}
			freeifaddrs(ifaddr);
		}
		sleep(5);
	}
	return NULL;
}

static void * ledthread_func(void * vptr_args) {
	struct ledthread_data * ltdata = (struct ledthread_data *)vptr_args;
	enum recv_state state;
	int fd;
	
	if ((fd = open("/dev/leds0", 0)) < 0)
		fd = open("/dev/leds", 0);
	if (fd < 0)
		printf("Unable to open leds controller\n");
	else
		printf("LED thread has started\n");
	
	while (1) {
		pthread_mutex_lock(&leddata);
		if ((state = *(ltdata->state)) == RECV_PLAYING_STREAM)
			*(ltdata->state) = RECV_READY;
		pthread_mutex_unlock(&leddata);
		
		if (state == RECV_READY) {
			if (fd >= 0)
				ioctl(fd, 0, 0);
			usleep(800000);
			if (fd >= 0)
				ioctl(fd, 1, 0);
			usleep(800000);
			continue;
		}
		
		if (state == RECV_PLAYING_STREAM) {
			if (fd >= 0)
				ioctl(fd, 0, 0);
			usleep(100000);
			if (fd >= 0)
				ioctl(fd, 1, 0);
			usleep(100000);
			continue;
		}
	}
	
	return 0;
}

// Return 0 if packet is invalid, 2 if stream is proceeding, 1 if stream is restarting
int parse_packet(char * packet, uint16_t plen, uint16_t session_id, uint16_t uuid,
	uint32_t * stream_frames, CELTDecoder * cd, celt_int16 * result) {
	uint16_t data_size;

	if (*(uint32_t *)(packet + 0) != SERVERMAGIC)
		return PACKET_INVALID;
	if (*(uint16_t *)(packet + 4) != uuid)
		return PACKET_INVALID;
	if (*(uint16_t *)(packet + 6) != session_id)
		return PACKET_INVALID;
		
	if (*(uint32_t *)(packet + 8) == 0) {
		if (*(uint16_t *)(packet + 12) == 0) {
			*stream_frames = 1;
			return PACKET_RESTART;
		} else
			return PACKET_INVALID; // Invalid frame
	} else if (*(uint32_t *)(packet + 8) >= *stream_frames) { // Next frame has arrived, drop delayed
		if ((data_size = *(uint16_t *)(packet + 12)) != (plen - 14))
			return PACKET_INVALID;

		++(*stream_frames);
		celt_decode(cd, packet + 14, data_size, result);
		return PACKET_PROCEED;
	}
	return PACKET_INVALID;
}


int main(int argc, char ** argv) {

	int is_debug = 0;
	celt_int16 * sample_buffer;
	unsigned char * encoded_buffer;
	int sample_rate;
	int sample_bits;
	int sample_channels;
	int sample_buffer_size;
	int sample_size;
	int k, s, slen = sizeof(struct sockaddr_in);

	uint64_t	count = 0, sum = 0, start = 0, curr;
	uint16_t	session_id;
	uint64_t	frames = 0;
	uint16_t	jitter = 0;
	uint16_t	mean = 0;
	uint32_t	stream_frames = 0;
	uint64_t	prev_intervals[20];
	uint64_t	jitter64;

	CELTMode * cm;
	CELTDecoder * cd;
	int compressed;
	
	struct sockaddr_in si_me, si_other;
	struct ring_buffer rbuf;
	struct thread_data tdata;
	struct bthread_data btdata;
	enum recv_state rstate;
	struct ledthread_data ledtdata;
	
	pthread_t thread, bcthread, ledthread;

	srand(time(NULL));
	session_id = rand() % 65000;

	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			++is_debug;
			printf("Running in debug mode\n");
		}
	}
   
	if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		diep("Unable to open listening UDP socket\n");

	memset((char *) &si_me, 0, sizeof(si_me));
	si_me.sin_family = AF_INET;
	si_me.sin_port = htons(PORT);
	si_me.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(s, (struct sockaddr *)(&si_me), sizeof(si_me)) == -1)
		diep("Unable to bind to listening UDP socket\n");

	
	setpriority(PRIO_PROCESS, 0, -20);	
	// Hardcoded parameters for DSP

	sample_rate = 44100; // Desc. frequency, Hz
	sample_bits = 16; // Bits per sample
	sample_channels = 2; // Stereo

	sample_size = 1024;
	sample_buffer_size =  sample_size * sample_bits/8 * sample_channels;
	sample_buffer = (celt_int16 *) calloc(sample_buffer_size, sizeof(char));
	encoded_buffer = (unsigned char *) calloc(sample_buffer_size, sizeof(char));

	// Initializing CELT decoder
	cm = celt_mode_create(sample_rate, sample_size, NULL);
	cd = celt_decoder_create(cm, sample_channels, NULL);

	// Creating ring buffer of 256 kbytes (64 samples)
	ring_buffer_create(&rbuf, BUFSIZE);

	// Filling thread structure
	tdata.device = aout_init();
	tdata.rbuf = &rbuf;
	tdata.sample_buffer_size = sample_buffer_size;
	tdata.is_debug = is_debug;

	btdata.uuid = 0;
	btdata.session_id = session_id;
	btdata.start = time(NULL);
	btdata.jitter = &jitter;
	btdata.mean = &mean;
	btdata.frames = &frames;
	
	rstate = RECV_READY;
	ledtdata.state = &rstate;

	if (pthread_create(&thread, NULL, thread_func, &tdata) != 0)
		diep("Unable to start playing thread\n");

	if (pthread_create(&bcthread, NULL, bcast_thread_func, &btdata) != 0)
		diep("Unable to start broadcasting thread\n");

	if (pthread_create(&ledthread, NULL, ledthread_func, &ledtdata) != 0)
		diep("Unable to start led thread\n");
   
	for (;;) {
		start = getcount();

		if ((k = recvfrom(s, encoded_buffer, sample_buffer_size, 0,
			(struct sockaddr *)(&si_other), &slen)) == -1)
			diep("Error during call recvfrom()");

		k = parse_packet(encoded_buffer, k, session_id, btdata.uuid,
				&stream_frames, cd, sample_buffer);
		if (k == PACKET_INVALID)
			goto drop_packet;
		if (k == PACKET_RESTART)
			goto restart_stream;
		
		pthread_mutex_lock(&leddata);
		rstate = RECV_PLAYING_STREAM;
		pthread_mutex_unlock(&leddata);

		if (is_debug)
			printf("Received %llu packet from %s:%d, data size: %d\n",
				(long long unsigned int)count,
				inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port), k);

		pthread_mutex_lock(&mdata);
		if (ring_buffer_count_length(&rbuf) < ((1UL << BUFSIZE) >> 12UL) - 1) {
			memcpy(ring_buffer_write_address(&rbuf), sample_buffer,
			       sample_buffer_size);
			ring_buffer_write_advance(&rbuf, sample_buffer_size);
			if (is_debug)
				printf("Buffer size:%lu b, elements:%lu\n",
					ring_buffer_count_bytes(&rbuf),
					ring_buffer_count_length(&rbuf));
		} else
			if (is_debug)
				printf("Frames arrive so fast, buffer overrun, skipping\n");
		pthread_mutex_unlock(&mdata);

		curr = getcount() - start;
		sum += curr;
		++count;
		
		pthread_mutex_lock(&bcdata);
		++frames;
		pthread_mutex_unlock(&bcdata);
		
		if (count < 20)
			prev_intervals[count] = curr;
		else {
			memmove((uint64_t *)&prev_intervals, (uint64_t *)&prev_intervals + 1,
				19 * sizeof(uint64_t));
			prev_intervals[19] = curr;
			
			
			pthread_mutex_lock(&bcdata);
			mean = (uint16_t)(floor(sum * 0.1F / count)) % 65000;		
		
			jitter = 0;
			for (k = 0; k < 20; ++k)
				jitter += (prev_intervals[k] - mean) * (prev_intervals[k] - mean);

			jitter = (uint16_t)(sqrtf(jitter / 20.0F) / 10.0F);
			pthread_mutex_unlock(&bcdata);
		}
		
		continue;
restart_stream:
		sum = 1;
		count = 0;
		
drop_packet:
		continue;
	}

	close(s);
	ring_buffer_free(&rbuf);
	return 0;
}

