/*
 * Elbereth sender daemon
 *
 *
 * Copyright (c) 2012 Ilya Ponetayev <ilya.ponetaev@kodep.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */
 
#include <arpa/inet.h>
#include <celt/celt.h>
#include <celt/celt_types.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "types.h"

#define SYSFREQ 1E6
#define BUFLEN 512
#define PORT 9930
#define BCASTPORT 9931
#define MAXBUFLEN 512
#define SENDERS 512
#define BCAST_SLEEP_SEC	10
#define HOLDTIME_SEC	30

struct sender {
	uint16_t uuid;
	uint16_t ssid;
	uint32_t uptime;

	uint16_t jitter;
	uint16_t mean;
	uint64_t frames;

	int64_t hold;
	unsigned finish;
	unsigned long addr;
	pthread_mutex_t sender_m;
	pthread_mutex_t finish_m;
	pthread_t pid;
};


uint64_t getcount(void) {
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec * SYSFREQ + t.tv_usec;
}


int fifo_exist(char * filename) {
	struct stat	buffer;   
	return ((stat (filename, &buffer) == 0) && S_ISFIFO(buffer.st_mode));
}

int file_exist(char * filename) {
	struct stat	buffer;   
	return ((stat (filename, &buffer) == 0) && S_ISREG(buffer.st_mode));
}


int max(int * arr, int count) {
	int i = 0, max = arr[0];
	while (i < count) {
		if (arr[i] > max)
			max = arr[i];
		++i;
	}
	return max;
}


void * threadfunc_sender_main (void * tdata) {
	char pipe[BUFLEN], pipeid[BUFLEN], cmd[BUFLEN];
	int i, j, s, sample_rate, sample_bits, sample_channels, sample_buffer_size;
	int sample_size, fdi, cmdlen, ipipeid, compressed, srv;
	unsigned char * encoded_buffer;
	unsigned long dest;
	
	int64_t		t;
	uint64_t	start, gcount;

	fd_set		rfds;
	FILE *		fpipeid;
	celt_int16 * sample_buffer;
	CELTMode * cm;
	CELTEncoder * ce;
	struct sockaddr_in si_other;
	struct timeval tv;

	int slen = sizeof(si_other), jitter = 0, is_debug = 0, stop = 0;
	double		dr = 0, de = 0;
	uint16_t	uuid = 0;
	uint16_t	session_id = 0;
	uint16_t	bitrate = 160;
	uint32_t	stream_frames = 0;
	
	pthread_mutex_lock(&((struct sender *)tdata)->sender_m);
	dest = ((struct sender *)tdata)->addr;
	uuid = ((struct sender *)tdata)->uuid;
	session_id = ((struct sender *)tdata)->ssid;
	pthread_mutex_unlock(&((struct sender *)tdata)->sender_m);
	
	
  	srand(time(NULL));

	if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		printf("[%s : %d : %d] Unable to open UDP sending socket\n", inet_ntoa(si_other.sin_addr), uuid, session_id);
		pthread_exit(NULL);
	}

	memset((char *) &si_other, 0, sizeof(si_other));
	si_other.sin_family = AF_INET;
	si_other.sin_port = htons(PORT);
	si_other.sin_addr.s_addr = dest;
	
	cmdlen = sprintf(pipe, "/tmp/elbereth-%s-%d-%d.sock", inet_ntoa(si_other.sin_addr), uuid, session_id);
	pipe[cmdlen] = '\0';
	pipeid[sprintf(pipeid, "%s.id", pipe)] = '\0';
	
	cmdlen = sprintf(cmd, "pactl load-module module-pipe-sink sink_name=Elbereth rate=44100 format=s16le file=%s > %s; sleep 3",
		pipe, pipeid);
	cmd[cmdlen] = '\0';
	system(cmd);
	
	if (!(fpipeid = fopen(pipeid, "r"))) {
		printf("[%s : %d : %d] Unable to open pipe id\n", inet_ntoa(si_other.sin_addr), uuid, session_id);
		pthread_exit(NULL);
	}
	fscanf(fpipeid, "%d", &ipipeid);
	fclose(fpipeid);
	
	printf("[%s : %d : %d] Pipe id is %d\n", inet_ntoa(si_other.sin_addr), uuid, session_id, ipipeid);
	
	
	if (!fifo_exist(pipe)) {
		printf("[%s : %d : %d] Pipe '%s' doesn't exist\n", inet_ntoa(si_other.sin_addr), uuid, session_id, pipe);
		pthread_exit(NULL);
	}
				
	if ((fdi = open(pipe, O_RDONLY)) < 0) {
		printf("[%s : %d : %d] Unable to open pipe for reading\n", inet_ntoa(si_other.sin_addr), uuid, session_id);
		pthread_exit(NULL);
	}
		
	printf("[%s : %d : %d] Start transmission with bitrate %u kbps\n", inet_ntoa(si_other.sin_addr), uuid, session_id, bitrate);

	// Hardcoded parameters for DSP

	sample_rate = 44100; // Desc. frequency, Hz
	sample_bits = 16; // Bits per sample
	sample_channels = 2; // Stereo

	sample_size = 1024;

	sample_buffer_size =  sample_size * sample_bits/8 * sample_channels;
	sample_buffer = (celt_int16 *) calloc(sample_buffer_size, sizeof(char));
	encoded_buffer = (unsigned char *) calloc(sample_buffer_size, sizeof(char));
	cm = celt_mode_create(sample_rate, sample_size, NULL);
	ce = celt_encoder_create(cm, sample_channels, NULL);

	celt_encoder_ctl(ce, CELT_SET_COMPLEXITY(10));
	celt_encoder_ctl(ce, CELT_SET_PREDICTION(2));
	celt_encoder_ctl(ce, CELT_SET_VBR_RATE(bitrate * 1000));

	*((uint32_t *)((char *)sample_buffer + 0)) = 0xE1BE5E54;
	*((uint16_t *)((char *)sample_buffer + 4)) = uuid;
	*((uint16_t *)((char *)sample_buffer + 6)) = session_id;
	*((uint32_t *)((char *)sample_buffer + 8)) = 0;
	*((uint16_t *)((char *)sample_buffer + 12)) = 0;	
		
	if (sendto(s, sample_buffer, 14, 0, (struct sockaddr *)&si_other, slen) == -1)
		pthread_exit(NULL);

	i = 0;
	start = getcount();
	while (!stop) {
		
		pthread_mutex_lock(&((struct sender *)tdata)->finish_m);
		stop = ((struct sender *)tdata)->finish;
		pthread_mutex_unlock(&((struct sender *)tdata)->finish_m);		
		++i;
		++stream_frames;
		
		j = 0;
		
		while (j < sample_size * 4) {
			    FD_ZERO(&rfds);
				FD_SET(fdi, &rfds);
				
				tv.tv_sec = 0;
				tv.tv_usec = 10;
				
				srv = select(fdi + 1, &rfds, NULL, NULL, &tv);
				
				if (srv < 0) {
					printf("[%s : %d : %d] Select() returned negative\n", inet_ntoa(si_other.sin_addr), uuid, session_id);
					pthread_exit(NULL);
				}
				if (srv && FD_ISSET(fdi, &rfds)) {
					srv = read(fdi, sample_buffer + j, 4096);
					
					if (srv == 0) {
						printf("[%s : %d : %d] Got EOF in pipe\n", inet_ntoa(si_other.sin_addr), uuid, session_id);
						pthread_exit(NULL);
					}
					
					if (srv < 0)
						pthread_exit(NULL);
					j += srv;
				}
		}
		compressed = celt_encode(ce, sample_buffer, NULL, encoded_buffer, 1024);
		
		memset(sample_buffer, 0, compressed + 10);
		
		*((uint32_t *)((char *)sample_buffer + 0)) = 0xE1BE5E54;
		*((uint16_t *)((char *)sample_buffer + 4)) = uuid;
		*((uint16_t *)((char *)sample_buffer + 6)) = session_id;
		*((uint32_t *)((char *)sample_buffer + 8)) = stream_frames;
		*((uint16_t *)((char *)sample_buffer + 12)) = compressed;
		memcpy((char *)sample_buffer + 14, encoded_buffer, compressed);
		
		if (sendto(s, sample_buffer, compressed + 14, 0, (struct sockaddr *)&si_other, slen) == -1) {
			printf("[%s : %d : %d] Unable to send handshake packet\n", inet_ntoa(si_other.sin_addr), uuid, session_id);
			pthread_exit(NULL);
		}

		usleep(sample_size * 1000000 / sample_rate + jitter);
		if (is_debug)
			printf("[%s : %d : %d] Planned time: %.2f ms, real time: %.2f ms, needed time: %.2f\n",
				inet_ntoa(si_other.sin_addr), uuid, session_id,
				(sample_size * 1000000 / sample_rate + jitter) / 1000.0F, (getcount() - start) / 1000.0F,
				(sample_size * 1000000 / sample_rate) / 1000.0F);
		if (i % 1000 == 0)
			printf("[%s : %d : %d] Sending %d packet: jitter bias: %d\n",
				inet_ntoa(si_other.sin_addr), uuid, session_id, i, jitter);

		gcount = getcount();

		dr += (gcount - start) / 1000.0F;
		de += (sample_size * 1000000 / sample_rate) / 1000.0F;
		if ((abs(t = ((sample_size * 1000000 / sample_rate) - (gcount - start))) >
			(int)(1E-4 * (sample_size * 1000000 / sample_rate)  )) ){
			if (t > 0 || (de > dr)) {
				jitter += (i < 100) ? 10 : (i < 200) ? 2 : 1;
				if (is_debug)
					printf("Sending packes too fast, increasing jitter: %d\n", jitter);
			} else {
				jitter -= (i < 100) ? 10 : (i < 200) ? 2 : 1;
				if (is_debug)
					printf("Sending packes too slow, decreasing jitter: %d\n", jitter);
			}
		}
		start = gcount;
	}
	printf("[%s : %d : %d] Got stop, cleaning up\n", inet_ntoa(si_other.sin_addr), uuid, session_id);

	close(s);
	close(fdi);
	
	cmdlen = sprintf(cmd, "pactl unload-module %d", ipipeid);
	cmd[cmdlen] ='\0';
	system(cmd);
	
	pthread_exit(NULL);
}



int main(int argc, char ** argv) {
	char buf[MAXBUFLEN], * pbuf = (char *)buf;
	int i, j, k, srv, numbytes;
	int sockets[SENDERS];
	
	uint64_t stime;
	
	fd_set rfds;
	struct ifaddrs *ifaddr, *ifa;
	struct timeval tv;
	struct sender senders[SENDERS];
	socklen_t addr_len;
	
	int sockets_n = 0, senders_n = 0;
	struct sockaddr_in sockets_a[SENDERS], their_addr;

	setpriority(PRIO_PROCESS, 0, -20);	

	if (!file_exist("/usr/bin/pacmd")) {
		printf("[main] Unable to work without pacmd from pulseaudio\n");
		exit(EXIT_FAILURE);
	} else
		printf("[main] OK, pacmd found\n");

	if (getifaddrs(&ifaddr) == -1) {
		printf("[main] Unable to enumerate interfaces\n");
		exit(EXIT_FAILURE);
	}
	
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
                   continue;

		if (ifa->ifa_addr->sa_family == AF_INET) {
			if ((sockets[sockets_n] = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
				printf("[main] Unable to open socket on interface %s\n",
					inet_ntoa(((struct sockaddr_in *)(ifa->ifa_ifu.ifu_broadaddr))->sin_addr));
				exit(EXIT_FAILURE);
			}

			printf("[main] Open listening socket on %s interface (fd = %d)\n",
				inet_ntoa(((struct sockaddr_in *)(ifa->ifa_ifu.ifu_broadaddr))->sin_addr), sockets[sockets_n]);
			sockets_a[sockets_n].sin_family = AF_INET;
			sockets_a[sockets_n].sin_addr = ((struct sockaddr_in *)(ifa->ifa_ifu.ifu_broadaddr))->sin_addr;
			sockets_a[sockets_n].sin_port = htons(BCASTPORT);
			memset(&(sockets_a[sockets_n].sin_zero), '\0', 8);
				
			if (bind(sockets[sockets_n], (struct sockaddr *)&(sockets_a[sockets_n]),
				sizeof(struct sockaddr)) == -1) {
				printf("[main] Unable to bind to socket on interface %s\n",
					inet_ntoa(((struct sockaddr_in *)(ifa->ifa_ifu.ifu_broadaddr))->sin_addr));
				exit(EXIT_FAILURE);
			}
			++sockets_n;
		}
	}

	printf("[main] Opened %d sockets\n", sockets_n);
	freeifaddrs(ifaddr);

	while (1) {
		stime = getcount();
		FD_ZERO(&rfds);
		for (i = 0; i < sockets_n; ++i)
			FD_SET(sockets[i], &rfds);

		tv.tv_sec = BCAST_SLEEP_SEC;
		tv.tv_usec = 0;
		srv = select(max(sockets, sockets_n) + 1, &rfds, NULL, NULL, &tv);

		if (srv < 0) {
			printf("[main] select() got negative result\n");
			exit(EXIT_FAILURE);
		}
		if (!srv)
			printf("[main] No packet came in 10 seconds\n");

		if (srv) {
			for (i = 0; i < sockets_n; ++i) {
				if (FD_ISSET(sockets[i], &rfds)) {
					addr_len = sizeof(struct sockaddr);
					if ((numbytes = recvfrom(sockets[i], buf, MAXBUFLEN - 1 , 0,
						(struct sockaddr *)&their_addr, &addr_len)) == -1) {
						printf("[main] Unable to recieve from UDP socket\n");
						exit(EXIT_FAILURE);
					}

					printf("[main] Receive packet from %s: ", inet_ntoa(their_addr.sin_addr));
					printf("UUID: %u, SSID: %u, uptime: %u secs, jitter: %u us, mean: %u us, frames: %"PRIu64"\n",
						*((uint16_t *)(pbuf + 4)), *((uint16_t *)(pbuf + 6)), *((uint32_t *)(pbuf + 8)),
						*((uint16_t *)(pbuf + 12)) * 10, *((uint16_t *)(pbuf + 14)) * 10,
						*((uint64_t *)(pbuf + 16)));

					k = 0;
					for (j = 0; j < senders_n; ++j) {
						if ((senders[j].uuid == *((uint16_t *)(pbuf + 4)))
							&& (senders[j].ssid == *((uint16_t *)(pbuf + 6)))
							&& (senders[j].addr == their_addr.sin_addr.s_addr)) {
							pthread_mutex_lock(&senders[j].sender_m);
							senders[j].uptime = *((uint32_t *)(pbuf + 8));
							senders[j].jitter = *((uint16_t *)(pbuf + 12));
							senders[j].mean = *((uint16_t *)(pbuf + 14));
							senders[j].frames = *((uint64_t *)(pbuf + 16));
							senders[j].hold = HOLDTIME_SEC * 1000000;
							pthread_mutex_unlock(&senders[j].sender_m);
							++k;
						}
					}

					if (!k) {
						senders[senders_n].uuid = *((uint16_t *)(pbuf + 4));
						senders[senders_n].ssid = *((uint16_t *)(pbuf + 6));
						senders[senders_n].addr = their_addr.sin_addr.s_addr;
						senders[senders_n].uptime = *((uint32_t *)(pbuf + 8));
						senders[senders_n].jitter = *((uint16_t *)(pbuf + 12));
						senders[senders_n].mean = *((uint16_t *)(pbuf + 14));
						senders[senders_n].frames = *((uint64_t *)(pbuf + 16));
						senders[senders_n].hold = HOLDTIME_SEC * 1000000;
						pthread_mutex_init(&senders[senders_n].sender_m, NULL);
						senders[senders_n].finish = 0;
						pthread_mutex_init(&senders[senders_n].finish_m, NULL);

						if (pthread_create(&senders[senders_n].pid, NULL, threadfunc_sender_main, &senders[senders_n]) != 0) {
							printf("[main] Unable to start sender's thread\n");
							exit(EXIT_FAILURE);
						}
						++senders_n;
					}
				}
			}
		}

		i = 0;
		while (i < senders_n) {
			if (senders[i].hold == -1 && ++i)
				continue;

			pthread_mutex_lock(&senders[i].sender_m);
			if ((senders[i].hold -= (getcount() - stime)) < 0) {
				pthread_mutex_unlock(&senders[i].sender_m);
				pthread_mutex_lock(&senders[i].finish_m);
				senders[i].finish = 1;
				pthread_mutex_unlock(&senders[i].finish_m);
				pthread_join(senders[i].pid, NULL);
				printf("[main] Thread has finished\n");
				senders[i].hold = -1;
			} else {
				pthread_mutex_unlock(&senders[i].sender_m);
				++i;
			}
		}
	}
	   
	exit(EXIT_SUCCESS);
}
