#include <sys/sendfile.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/param.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include <pthread.h>

#define __USE_GNU // for splice constants, SPLICE_F_MOVE, SPLICE_F_MORE
#include <fcntl.h>
#include "send_file.h"

static int local_sockfd = -1;
static int allow_checksum_skip_flag = 0;
static int always_accept_flag = 0;

static int validate_protocol_welcome_header(const char* buf, size_t buf_size) {
	int id;
	memcpy(&id, buf, sizeof(id));

	if (id != protocol_id) { return -1; }
	return 0;
}

static char *get_available_filename(const char* orig_filename) {
	char name_buf[128];
	strcpy(name_buf, orig_filename);
	name_buf[strlen(orig_filename)] = '\0';
	int num = 1;
	while (access(name_buf, F_OK) != -1) {
		// file exists, rename using the (#) scheme
		int bytes = sprintf(name_buf, "%s(%d)", orig_filename, num);
		name_buf[bytes] = '\0';
		++num;
	}
	return strdup(name_buf);
}


static int get_headerinfo(const char* buf, size_t buf_size, HEADERINFO *h) {
	memset(h, 0, sizeof(HEADERINFO));
	long accum = 0;
	memcpy(&h->protocol_id, buf, sizeof(h->protocol_id));
	accum += sizeof(h->protocol_id);

	memcpy(&h->filesize, buf + accum, sizeof(h->filesize));
	accum += sizeof(h->filesize);

	memcpy(&h->sha1_included, buf+accum, sizeof(h->sha1_included));
	accum += sizeof(h->sha1_included);

	h->filename = strdup(buf + accum);
	if (!h->filename) { 
		fprintf(stderr, "Error: extracting file name from header info failed. Bad header?\n");
		return -1;
	}
	int name_len = strlen(h->filename);
	accum += name_len + 1;
	memcpy(&h->sha1[0], buf + accum, SHA_DIGEST_LENGTH);

	return 0;


}	

static int consolidate(int out_sockfd, int flag) {
	char hacknowledge_buffer[8];
	memcpy(hacknowledge_buffer, &protocol_id, sizeof(protocol_id));
	memcpy(hacknowledge_buffer+sizeof(protocol_id), &flag, sizeof(flag));	
	int sent_bytes = send(out_sockfd, hacknowledge_buffer, 8, 0);
	if (sent_bytes <= 0) { 
		fprintf(stderr, "consolidate failed (send()): %s\n", strerror(errno));
	}
	return 0;
}

static double get_us(const struct timeval *beg) {
	struct timeval end;
	memset(&end, 0, sizeof(end));
	gettimeofday(&end, NULL);
	double microseconds = (end.tv_sec*1000000 + end.tv_usec) - (beg->tv_sec*1000000 + beg->tv_usec);
	return microseconds;
}

typedef struct _progress_struct {
	const long *cur_bytes;
	const long *total_bytes;
	const struct timeval *beg;
} progress_struct;

static void print_progress(long cur_bytes, long total_bytes, const struct timeval *beg) {
	
	static const char* esc_composite_clear_line_reset_left = "\r\033[0K";	// ANSI X3.64 magic
	fprintf(stderr, "%s", esc_composite_clear_line_reset_left);

	float progress = 100*(float)(cur_bytes)/(float)(total_bytes);

	// MB/s = (bytes/2^20) : (microseconds/1000000)
	// == (bytes/1048576) * (1000000/microseconds)
	// == (1000000/1048576) * (bytes/microseconds)
	static const float MB_us_coeff = 1000000.0/1048576.0;

	float rate = MB_us_coeff*((float)cur_bytes)/get_us(beg);	
	fprintf(stderr, "%lu/%lu bytes received (%.2f %%, %.2f MB/s)", cur_bytes, total_bytes, progress, rate);

}

void *progress_callback(void *progress) {

	progress_struct *p = (progress_struct*)progress;

	while (*p->cur_bytes < *p->total_bytes) {
		long cur_bytes = *p->cur_bytes;
		long total_bytes = *p->total_bytes;
		print_progress(cur_bytes, total_bytes, p->beg);
		sleep(1);
	}
	
	return NULL;
}

static long recv_file(int remote_sockfd, int *pipefd, int outfile_fd, long file_size) {
	long total_bytes_processed = 0;	
	struct timeval tv_beg;
	memset(&tv_beg, 0, sizeof(tv_beg));
	gettimeofday(&tv_beg, NULL);

	progress_struct p;

	p.cur_bytes = &total_bytes_processed;
	p.total_bytes = &file_size;
	p.beg = &tv_beg;
	pthread_t t1;
	pthread_create(&t1, NULL, progress_callback, (void*)&p);

	while (total_bytes_processed < file_size) {
		static const int max_chunksize = 16384;
		static const int spl_flag = SPLICE_F_MORE | SPLICE_F_MOVE;

		long bytes_recv = 0;
		long bytes = 0;

		long would_process = file_size - total_bytes_processed;
	       	long gonna_process = MIN(would_process, max_chunksize);

		// splice to pipe write head
		if ((bytes = 
		splice(remote_sockfd, NULL, pipefd[1], NULL, gonna_process, spl_flag)) <= 0) {
			fprintf(stderr, "socket->pipe_write splice returned %ld: %s\n", bytes_recv, strerror(errno));
			return -1;
		}
		// splice from pipe read head to file fd
		bytes_recv += bytes;

		int bytes_in_pipe = bytes_recv;
		int bytes_written = 0;
		while (bytes_in_pipe > 0) {
			if ((bytes_written = 
			splice(pipefd[0], NULL, outfile_fd, &total_bytes_processed, bytes_in_pipe, spl_flag)) <= 0) {
				fprintf(stderr, "pipe_read->file_fd splice returned %d: %sn", bytes_written, strerror(errno));
				return -1;
			}

			bytes_in_pipe -= bytes_written;

		}
	}
	if (total_bytes_processed != file_size) {
		fprintf(stderr, "warning: total_bytes_processed != file_size!\n");
	}

	pthread_join(t1, NULL);
	print_progress(total_bytes_processed, file_size, &tv_beg);

	double seconds = get_us(&tv_beg)/1000000.0;
	double MBs = get_megabytes(total_bytes_processed)/seconds;

	fprintf(stderr, "\nReceived %.2f MB in %.3f seconds (%.2f MB/s).\n\n", get_megabytes(total_bytes_processed), seconds, MBs);

	return total_bytes_processed;

}

static void make_lowercase(char *arr, int length) {
	int i = 0;
	for (; i < length; ++i) {
		arr[i] = tolower(arr[i]);
	}
}

static int ask_user_consent() {
	fprintf(stderr, "Is this ok? [y/N] ");
	char buffer[128];
	buffer[127] = '\0';
	int index;
	char c;
get_answer:
	index = 0;
	while ((c = getchar()) != '\n') {
		buffer[index] = c;
		if (index < 127) {
			++index;
		}
	}
	
	buffer[index] = '\0';
	make_lowercase(buffer, index);

	if (strcmp(buffer, "y") == 0) {
		return 1;
	}
	else if (strcmp(buffer, "n") == 0) {
		return 0;
	}
	else { 
		fprintf(stderr, "Unknown answer \"%s\". [y/N]?", buffer);
		goto get_answer; // ;)
	} 

}

static void cleanup() {
	close(local_sockfd);
}

void signal_handler(int sig) {
	if (sig == SIGINT) {
		fprintf(stderr, "Received SIGINT. Aborting.\n");
		cleanup();
		exit(1);
	}
}

int main(int argc, char* argv[]) {

	int c;
	while ((c = getopt(argc, argv, "ac")) != -1) {
		switch(c) {
			case 'a':
				fprintf(stderr, "-a provided -> always accepting file transfers without asking for consent.\n");
				always_accept_flag = 1;
				break;
	
			case 'c':
				fprintf(stderr, "-c provided -> allowing program to skip checksum verification.\n");
				allow_checksum_skip_flag = 1;
				break;
			case '?':
				fprintf(stderr, "warning: unknown option \'-%c\n\'", optopt);
				break;
			default:
				abort();
		}
	}
	
	struct sigaction new_action, old_action;
	new_action.sa_handler = signal_handler;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	sigaction(SIGINT, NULL, &old_action);
	if (old_action.sa_handler != SIG_IGN) {
		sigaction(SIGINT, &new_action, NULL);
	}

	struct sockaddr_in local_saddr, remote_saddr;

	local_sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (local_sockfd < 0) {
		fprintf(stderr, "socket() failed.\n");
		return 1;
	}

	memset(&local_saddr, 0, sizeof(local_saddr));
	memset(&remote_saddr, 0, sizeof(remote_saddr));

	local_saddr.sin_family = AF_INET;
	local_saddr.sin_addr.s_addr = INADDR_ANY;
	local_saddr.sin_port = htons(port);

	if (bind(local_sockfd, (struct sockaddr*) &local_saddr, sizeof(local_saddr)) < 0) {
		fprintf(stderr, "bind() failed: %s\n", strerror(errno));
		return 1;
	}
	fprintf(stderr, "send_file server.\n");

	print_ip_addresses();
	fprintf(stdout, "bind() on port %d.\n", port);
	listen(local_sockfd, 5);

	char handshake_buffer[128];

	while (1) {
		fprintf(stderr, "\nListening for incoming connections.\n");

		int remote_sockfd = -1;
		int outfile_fd = -1;

		socklen_t remote_saddr_size = sizeof(remote_saddr);
		remote_sockfd = accept(local_sockfd, (struct sockaddr *) &remote_saddr, &remote_saddr_size);
		if (remote_sockfd < 0) {
			fprintf(stderr, "warning: accept() failed.\n");
		}

		char ip_buf[32];
		inet_ntop(AF_INET, &(remote_saddr.sin_addr), ip_buf, sizeof(ip_buf));
		printf("Client connected from %s.\n", ip_buf);

		int received_bytes = 0;	
		int handshake_len = recv(remote_sockfd, handshake_buffer, sizeof(handshake_buffer), 0);
		if (handshake_len <= 0) {
			fprintf(stderr, "error: handshake_len <= 0\n");
			close(remote_sockfd); 
			continue;
		}
		received_bytes += handshake_len;

		if (validate_protocol_welcome_header(handshake_buffer, handshake_len) < 0) {
			fprintf(stderr, "warning: validate_protocol_welcome_header failed!\n");
			consolidate(remote_sockfd, HANDSHAKE_FAIL);
			goto cleanup;
		}

		HEADERINFO h;

		if (get_headerinfo(handshake_buffer, handshake_len, &h) < 0) {
			fprintf(stderr, "error: headerinfo error!\n");
			consolidate(remote_sockfd, HANDSHAKE_FAIL);
			goto cleanup;
		}

		if (h.sha1_included == 0 && allow_checksum_skip_flag == 0) {
			fprintf(stderr, "error: client didn't provide a sha1 hash for the input file (-c was used; use -c on the server to allow this). Rejecting.\n");
			consolidate(remote_sockfd, HANDSHAKE_CHECKSUM_REQUIRED);
			goto cleanup;
		}

		int pipefd[2];

		if (pipe(pipefd) < 0) {
			fprintf(stderr, "pipe() error.\n");
			consolidate(remote_sockfd, HANDSHAKE_FAIL);
			goto cleanup;
		}

		char *name = get_available_filename(h.filename);

		fprintf(stderr, "The client wants to send the file %s (size %.2f MB).\n", h.filename, get_megabytes(h.filesize)); 

		if (always_accept_flag == 0) {
			if (!ask_user_consent()) { consolidate(remote_sockfd, HANDSHAKE_DENIED); goto cleanup; }
		}

		fprintf(stderr, "Writing to output file %s.\n\n", name);
		outfile_fd = open(name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
		free(name);

		if (outfile_fd < 0) {
			fprintf(stderr, "open() failed (errno: %s).\n", strerror(errno));
			consolidate(remote_sockfd, HANDSHAKE_FAIL);
			goto cleanup;
		}

		// inform the client program that they can start blasting dat file data
		consolidate(remote_sockfd, HANDSHAKE_OK);

		long ret;
		if ((ret = recv_file(remote_sockfd, pipefd, outfile_fd, h.filesize)) < h.filesize) {
			if (ret < 0) {
				fprintf(stderr, "recv_file failure.\n");
			}
			else {
				fprintf(stderr, "recv_file: warning: received data size (%ld) is less than expected (%lu)!\n", ret, h.filesize);
			}
			goto cleanup;
		}
		
		unsigned char* block = mmap(NULL, h.filesize, PROT_READ, MAP_SHARED, outfile_fd, 0);
		if (block == MAP_FAILED) {
			fprintf(stderr, "mmap on outfile_fd failed: %s.\n", strerror(errno));
			return 1;
		}

		if (h.sha1_included && allow_checksum_skip_flag == 0) {
			fprintf(stderr, "Calculating sha1 sum...\n\n");
			unsigned char* sha1_received = get_sha1(block, h.filesize);
			munmap(block, h.filesize);
			
			if (compare_sha1(h.sha1, sha1_received) < 0) {
				return 1;
			}

			free(sha1_received);
		}
		else {
			fprintf(stderr, "(skipping checksum verification)\n\n");
		}
		fprintf(stderr, "Success. ");

		free(h.filename);

	cleanup:
		close(remote_sockfd);
		close(outfile_fd);

	}

	cleanup();

	return 0;
}
