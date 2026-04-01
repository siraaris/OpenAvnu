/*************************************************************************************************************
Copyright (c) 2016-2017, Harman International Industries, Incorporated
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS LISTED "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS LISTED BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*************************************************************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <sys/wait.h>

#define SHAPER_LOG_COMPONENT "Main"
#include "shaper_log.h"

#define STREAMDA_LENGTH 18
#define SHAPER_PORT 15365 /* Unassigned at https://www.iana.org/assignments/port-numbers */
#define MAX_CLIENT_CONNECTIONS 10
#define COMMAND_BUFFER_SIZE 2048
#define USER_COMMAND_PROMPT "\nEnter the command:  "

typedef struct cmd_ip
{
	int reserve_bw;
	int unreserve_bw;
	char interface[IFNAMSIZ];
	int class_a, class_b;
	int measurement_interval;//usec
	int max_frame_size;
	int max_frame_interval;
	int link_speed_mbps;
	char stream_da[STREAMDA_LENGTH];
	int delete_qdisc;
	int quit;
} cmd_ip;

typedef enum {
	SHAPER_CLASS_A = 1,
	SHAPER_CLASS_B = 2,
} shaper_class_t;

typedef enum {
	SHAPER_QDISC_CBS = 0,
	SHAPER_QDISC_ETF = 1,
	SHAPER_QDISC_CBS_ETF = 2,
} shaper_qdisc_mode_t;

typedef struct stream_da
{
	char dest_addr[STREAMDA_LENGTH];
	int bandwidth_bps;
	int max_frame_size;
	shaper_class_t class_type;
	int owner_sockfd;
	struct stream_da *next;
} stream_da;

stream_da *head = NULL;
int sr_classa=0, sr_classb=0;
int daemonize=0,c=0;
char interface[IFNAMSIZ] = {0};
int bandwidth_bps = 0;
int classa_parent = 2, classb_parent=3;
// mqprio creates classids 1:1..1:4 for num_tc=4.
// Keep queue 0 for gPTP/control; steer Class A to queue 1 and Class B to queue 2.
char classa_parent_str[8] = "1:2";
char classb_parent_str[8] = "1:3";
int classa_bw = 0, classb_bw = 0;
int classa_max_frame_size = 0, classb_max_frame_size = 0;
int classa_preset_bw = 0, classb_preset_bw = 0;
int classa_preset_max_frame_size = 0, classb_preset_max_frame_size = 0;
int classa_preset_active = 0, classb_preset_active = 0;
int link_speed_mbps = 0;
int skip_root_qdisc = 0;
int interface_cleaned = 0;
char tc_path[128] = "tc";
int tc_cbs_checked = 0;
int tc_etf_checked = 0;
int log_tc_commands = 0;
char modprobe_path[128] = "modprobe";
int root_qdisc_installed = 0;
int exit_received = 0;
int egress_qmap_enable = 0;
int qmap_classa = -1;
int qmap_classb = -1;
int qmap_default = 3;
int qmap_gptp = 0;
shaper_qdisc_mode_t classa_qdisc_mode = SHAPER_QDISC_CBS;
shaper_qdisc_mode_t classb_qdisc_mode = SHAPER_QDISC_CBS;
int classa_cbs_offload = 0;
long long classa_etf_delta_ns = 300000;
int classa_etf_offload = 1;
int classa_etf_skip_sock_check = 0;
int classb_cbs_offload = 0;
long long classb_etf_delta_ns = 300000;
int classb_etf_offload = 0;
int classb_etf_skip_sock_check = 0;
// Default mqprio traffic-class map:
//   prio 7/6 -> tc0 (gPTP/control, highest)
//   prio 3   -> tc1 (Class A)
//   prio 2   -> tc2 (Class B)
//   all others -> tc3 (default/best effort)
// gPTP ethertype is also pinned by flower filter when egress qmap is enabled.
#define SHAPER_TC_MAP_DEFAULT "3 3 2 1 3 3 0 0 3 3 3 3 3 3 3 3"
char shaper_tc_map[64] = SHAPER_TC_MAP_DEFAULT;

static int process_command(int sockfd, char command[]);
static int process_commands_from_stream(int sockfd, char *accum, int *accum_len, const char *chunk, int chunk_len);
static void cleanup_client_streams(int sockfd);
static int compute_cbs_params(int port_rate_bps, int idleslope_bps, int max_frame_size,
	int *sendslope_bps, int *hicredit, int *locredit);
static int run_cmd(const char *cmd, int log_failure);
static const char *qdisc_mode_name(shaper_qdisc_mode_t mode);
static int ensure_root_qdisc_installed(int sockfd, const char *ifname);
static void preinit_interface_qdisc(const char *ifname);
static void preseed_class_qdiscs(const char *ifname);
static void apply_seed_class_qdisc(int sockfd, const char *ifname, shaper_class_t class_type);
static void tc_cbs_command(int sockfd, char interface[], const char *parent, int handle,
	int idleslope_bps, int sendslope_bps, int hicredit, int locredit, int offload);
static void tc_etf_command(int sockfd, char interface[], const char *parent, int handle,
	long long delta_ns, int offload, int skip_sock_check);
static void tc_cbs_etf_command(int sockfd, char interface[], const char *parent, int handle,
	int idleslope_bps, int sendslope_bps, int hicredit, int locredit, int cbs_offload,
	long long delta_ns, int etf_offload, int etf_skip_sock_check);
static int parse_parent_queue_index(const char *parent);
static void apply_egress_qmap_filters(const char *ifname);

static void signal_handler(int signal)
{
	if (signal == SIGINT || signal == SIGTERM) {
		if (!exit_received) {
			SHAPER_LOG_INFO("Shutdown signal received");
			exit_received = 1;
		}
		else {
			// Force shutdown
			exit(2);
		}
	}
	else if (signal == SIGUSR1) {
		SHAPER_LOG_DEBUG("Waking up streaming thread");
	}
	else {
		SHAPER_LOG_ERROR("Unexpected signal");
	}
}

void log_client_error_message(int sockfd, const char *fmt, ...)
{
	static char error_msg[200], combined_error_msg[250];
	va_list args;

	if (SHAPER_LOG_LEVEL_ERROR > SHAPER_LOG_LEVEL)
	{
		return;
	}

	/* Get the error message. */
	va_start(args, fmt);
	vsprintf(error_msg, fmt, args);
	va_end(args);

	if (sockfd < 0)
	{
		/* Received from stdin.  Just log this error. */
		SHAPER_LOG_ERROR(error_msg);
	}
	else
	{
		/* Log this as a remote client error. */
		sprintf(combined_error_msg, "Client %d: %s", sockfd, error_msg);
		SHAPER_LOG_ERROR(combined_error_msg);

		/* Send the error message to the client. */
		sprintf(combined_error_msg, "ERROR: %s\n", error_msg);
		send(sockfd, combined_error_msg, strlen(combined_error_msg), 0);
	}
}

void log_client_debug_message(int sockfd, const char *fmt, ...)
{
	static char debug_msg[200], combined_debug_msg[250];
	va_list args;

	if (SHAPER_LOG_LEVEL_DEBUG > SHAPER_LOG_LEVEL)
	{
		return;
	}

	/* Get the debug message. */
	va_start(args, fmt);
	vsprintf(debug_msg, fmt, args);
	va_end(args);

	if (sockfd < 0)
	{
		/* Received from stdin.  Just log this message. */
		SHAPER_LOG_DEBUG(debug_msg);
	}
	else
	{
		/* Log this as a remote client message. */
		sprintf(combined_debug_msg, "Client %d: %s", sockfd, debug_msg);
		SHAPER_LOG_DEBUG(combined_debug_msg);

		/* Send the message to the client. */
		sprintf(combined_debug_msg, "DEBUG: %s\n", debug_msg);
		send(sockfd, combined_debug_msg, strlen(combined_debug_msg), 0);
	}
}

int is_empty()
{
	return head == NULL;
}

static int process_commands_from_stream(int sockfd, char *accum, int *accum_len, const char *chunk, int chunk_len)
{
	int i;
	int start = 0;
	int ret = 1;

	if (!accum || !accum_len || !chunk || chunk_len <= 0)
	{
		return ret;
	}

	// Append new data to the per-source accumulation buffer.
	if (*accum_len + chunk_len >= COMMAND_BUFFER_SIZE)
	{
		SHAPER_LOG_WARNING("Incoming command data exceeded buffer; dropping partial command");
		*accum_len = 0;
	}
	if (chunk_len >= COMMAND_BUFFER_SIZE)
	{
		chunk += (chunk_len - (COMMAND_BUFFER_SIZE - 1));
		chunk_len = COMMAND_BUFFER_SIZE - 1;
	}
	memcpy(accum + *accum_len, chunk, chunk_len);
	*accum_len += chunk_len;
	accum[*accum_len] = '\0';

	// Process complete newline-delimited commands only.
	for (i = 0; i < *accum_len; ++i)
	{
		if (accum[i] == '\n' || accum[i] == '\r')
		{
			accum[i] = '\0';
			char *cmd = &accum[start];

			while (*cmd && isspace(*cmd))
			{
				cmd++;
			}
			char *end = cmd + strlen(cmd);
			while (end > cmd && isspace(*(end - 1)))
			{
				*(--end) = '\0';
			}

			if (*cmd)
			{
				SHAPER_LOGF_INFO("The received command is \"%s\"", cmd);
				ret = process_command(sockfd, cmd);
				if (!ret)
				{
					*accum_len = 0;
					accum[0] = '\0';
					return 0;
				}
			}
			start = i + 1;
		}
	}

	// Keep any trailing partial line for the next recv() call.
	if (start > 0)
	{
		int remaining = *accum_len - start;
		if (remaining > 0)
		{
			memmove(accum, accum + start, remaining);
		}
		*accum_len = remaining;
		accum[*accum_len] = '\0';
	}

	return ret;
}

void insert_stream_da(int sockfd, char dest_addr[], int bandwidth_bps, int max_frame_size, shaper_class_t class_type)
{
	stream_da *node = (stream_da *)malloc(sizeof(stream_da));
	if (node == NULL)
	{
		log_client_error_message(sockfd, "Unable to allocate memory. Exiting program");
		shaperLogExit();
		exit(1);
	}
	strcpy(node->dest_addr, dest_addr);
	node->bandwidth_bps = bandwidth_bps;
	node->max_frame_size = max_frame_size;
	node->class_type = class_type;
	node->owner_sockfd = sockfd;
	node->next = NULL;
	if (is_empty())
	{
		head = node;
	}
	else
	{
		stream_da *current = head;
		while (current->next != NULL)
		{
			current = current->next;
		}
		current->next = node;
	}
}

static void recompute_class_totals(shaper_class_t class_type, int *total_bw, int *max_frame_size)
{
	stream_da *current = head;
	int bw = 0;
	int max_frame = 0;
	while (current != NULL)
	{
		if (current->class_type == class_type)
		{
			bw += current->bandwidth_bps;
			if (current->max_frame_size > max_frame)
			{
				max_frame = current->max_frame_size;
			}
		}
		current = current->next;
	}
	*total_bw = bw;
	*max_frame_size = max_frame;
}

static int class_preset_enabled(shaper_class_t class_type)
{
	if (class_type == SHAPER_CLASS_A)
	{
		return classa_preset_active;
	}
	else if (class_type == SHAPER_CLASS_B)
	{
		return classb_preset_active;
	}
	return 0;
}

int check_stream_da(int sockfd, char dest_addr[])
{
	stream_da *current = head;
	while (current != NULL)
	{
		if (!strcmp(current->dest_addr, dest_addr))
		{
			log_client_error_message(sockfd, "Stream DA already present");
			return 1;
		}
		current = current->next;
	}
	return 0;
}

stream_da* get_stream_da(int sockfd, char dest_addr[])
{
	stream_da *current = head;
	while (current != NULL)
	{
		if (!strcmp(current->dest_addr, dest_addr))
		{

			return current;
		}
		current = current->next;
	}
	log_client_error_message(sockfd, "Unknown Stream DA");
	return NULL;
}

void remove_stream_da(int sockfd, char dest_addr[])
{
	stream_da *current = head;
	(void) sockfd;

	if (current != NULL && !strcmp(current->dest_addr, dest_addr))
	{
		head = current->next;
		free(current);
		return;
	}

	while (current->next != NULL)
	{
		if (!strcmp(current->next->dest_addr, dest_addr))
		{
			stream_da *temp = current->next;
			current->next = current->next->next;
			free(temp);
			return;
		}
		current = current->next;
	}
}

void delete_streamda_list()
{
	stream_da *current = head;
	stream_da *next = NULL;
	while (current != NULL)
	{
		next = current->next;
		free(current);
		current = next;
	}
	head = NULL;
}

static void cleanup_client_streams(int sockfd)
{
	stream_da *current = head;
	stream_da *prev = NULL;
	int removed = 0;
	int sendslope_bps = 0, hicredit = 0, locredit = 0;

	if (sockfd < 0) {
		return;
	}

	while (current != NULL)
	{
		if (current->owner_sockfd == sockfd)
		{
			stream_da *to_free = current;
			if (prev == NULL) {
				head = current->next;
			}
			else {
				prev->next = current->next;
			}
			current = current->next;
			free(to_free);
			removed++;
			continue;
		}
		prev = current;
		current = current->next;
	}

	if (removed == 0) {
		return;
	}

	SHAPER_LOGF_INFO("Removed %d stale reservation(s) for closed socket %d", removed, sockfd);
	recompute_class_totals(SHAPER_CLASS_A, &classa_bw, &classa_max_frame_size);
	recompute_class_totals(SHAPER_CLASS_B, &classb_bw, &classb_max_frame_size);

	if (interface[0] == '\0') {
		return;
	}

	if (classa_bw == 0)
	{
		apply_seed_class_qdisc(-1, interface, SHAPER_CLASS_A);
		sr_classa = 0;
	}
	else if (classa_qdisc_mode == SHAPER_QDISC_ETF)
	{
		tc_etf_command(-1, interface, classa_parent_str, classa_parent,
			classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
		sr_classa = 1;
	}
	else if (classa_qdisc_mode == SHAPER_QDISC_CBS_ETF)
	{
		if (compute_cbs_params(link_speed_mbps * 1000000, classa_bw, classa_max_frame_size,
			&sendslope_bps, &hicredit, &locredit) == 0)
		{
			tc_cbs_etf_command(-1, interface, classa_parent_str, classa_parent,
				classa_bw, sendslope_bps, hicredit, locredit, classa_cbs_offload,
				classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
			sr_classa = 1;
		}
	}
	else if (compute_cbs_params(link_speed_mbps * 1000000, classa_bw, classa_max_frame_size,
		&sendslope_bps, &hicredit, &locredit) == 0)
	{
		tc_cbs_command(-1, interface, classa_parent_str, classa_parent,
			classa_bw, sendslope_bps, hicredit, locredit, classa_cbs_offload);
		sr_classa = 1;
	}

	if (classb_bw == 0)
	{
		apply_seed_class_qdisc(-1, interface, SHAPER_CLASS_B);
		sr_classb = 0;
	}
	else if (compute_cbs_params(link_speed_mbps * 1000000, classb_bw, classb_max_frame_size,
		&sendslope_bps, &hicredit, &locredit) == 0)
	{
		if (classb_qdisc_mode == SHAPER_QDISC_ETF)
		{
			tc_etf_command(-1, interface, classb_parent_str, classb_parent,
				classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
			sr_classb = 1;
		}
		else if (classb_qdisc_mode == SHAPER_QDISC_CBS_ETF)
		{
			tc_cbs_etf_command(-1, interface, classb_parent_str, classb_parent,
				classb_bw, sendslope_bps, hicredit, locredit, classb_cbs_offload,
				classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
			sr_classb = 1;
		}
		else
		{
			tc_cbs_command(-1, interface, classb_parent_str, classb_parent,
				classb_bw, sendslope_bps, hicredit, locredit, classb_cbs_offload);
			sr_classb = 1;
		}
	}
}

void usage (int sockfd)
{
	const char *usage = "Usage:\n"
			"	-r	Reserve Bandwidth\n"
			"	-u	Unreserve Bandwidth\n"
			"	-i	Interface\n"
			"	-c	Class ('A' or 'B')\n"
			"	-s	Measurement interval (in microseconds)\n"
			"	-b	Maximum frame size (in bytes)\n"
			"	-f	Maximum frame interval\n"
			"	-l	Link speed (in Mbps, required for CBS shaping)\n"
			"	-a	Stream Destination Address\n"
			"	-d	Delete qdisc\n"
			"	-q	Quit Application\n"
			"Reserving Bandwidth Example:\n"
			"	-ri eth2 -c A -s 125 -b 74 -f 1 -l 1000 -a ff:ff:ff:ff:ff:11\n"
			"Unreserving Bandwidth Example:\n"
			"	-ua ff:ff:ff:ff:ff:11\n"
			"Quit Example:\n"
			"	-q\n"
			"Delete qdisc Example:\n"
			"	-d\n";
	if (sockfd < 0)
	{
		printf("%s",usage);
	}
	else
	{
		send(sockfd, usage, strlen(usage),0);
	}

}

cmd_ip parse_cmd(char command[])
{
	cmd_ip inputs = {0};
	char *saveptr = NULL;
	char *token = strtok_r(command, " \t", &saveptr);

	while (token != NULL)
	{
		if (token[0] == '-')
		{
			char *opt = token + 1;
			while (*opt != '\0')
			{
				char flag = *opt++;
				char *value = NULL;

				switch (flag)
				{
					case 'r':
						inputs.reserve_bw = 1;
						continue;
					case 'u':
						inputs.unreserve_bw = 1;
						continue;
					case 'd':
						inputs.delete_qdisc = 1;
						continue;
					case 'q':
						inputs.quit = 1;
						continue;
					case 'i':
					case 'c':
					case 's':
					case 'b':
					case 'f':
					case 'l':
					case 'a':
						break;
					default:
						// Unknown option character, ignore to preserve compatibility.
						continue;
				}

				// Options with values accept either "-x value" or "-xvalue".
				if (*opt != '\0') {
					value = opt;
					opt += strlen(opt);
				}
				else {
					value = strtok_r(NULL, " \t", &saveptr);
				}

				if (value == NULL) {
					break;
				}

				switch (flag)
				{
					case 'i':
						strncpy(inputs.interface, value, IFNAMSIZ - 1);
						inputs.interface[IFNAMSIZ - 1] = '\0';
						break;
					case 'c':
						if (value[0] == 'A' || value[0] == 'a') {
							inputs.class_a = 1;
						}
						else if (value[0] == 'B' || value[0] == 'b') {
							inputs.class_b = 1;
						}
						break;
					case 's':
						inputs.measurement_interval = atoi(value);
						break;
					case 'b':
						inputs.max_frame_size = atoi(value);
						break;
					case 'f':
						inputs.max_frame_interval = atoi(value);
						break;
					case 'l':
						inputs.link_speed_mbps = atoi(value);
						break;
					case 'a':
						strncpy(inputs.stream_da, value, STREAMDA_LENGTH - 1);
						inputs.stream_da[STREAMDA_LENGTH - 1] = '\0';
						break;
				}

				// Value consumed; no more flags can be parsed from this token.
				break;
			}
		}
		token = strtok_r(NULL, " \t", &saveptr);
	}

	return inputs;
}

static int compute_cbs_params(int port_rate_bps, int idleslope_bps, int max_frame_size,
	int *sendslope_bps, int *hicredit, int *locredit)
{
	long long sendslope = 0;
	long long hi = 0;
	long long lo = 0;
	if (port_rate_bps <= 0 || idleslope_bps <= 0 || max_frame_size <= 0)
	{
		return -1;
	}
	if (idleslope_bps >= port_rate_bps)
	{
		return -1;
	}
	sendslope = (long long)idleslope_bps - (long long)port_rate_bps;
	hi = ((long long)idleslope_bps * (long long)max_frame_size) / ((long long)port_rate_bps - (long long)idleslope_bps);
	lo = (sendslope * (long long)max_frame_size) / (long long)port_rate_bps;
	*sendslope_bps = (int)sendslope;
	*hicredit = (int)hi;
	*locredit = (int)lo;
	return 0;
}

static void cleanup_qdisc(const char *ifname)
{
	char tc_command[1000]={0};
	if (!ifname || ifname[0] == '\0')
	{
		return;
	}

	// Remove clsact filters/qdisc if present; these otherwise linger across runs.
	snprintf(tc_command, sizeof(tc_command), "%s filter del dev %s egress >/dev/null 2>&1", tc_path, ifname);
	if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
	log_client_debug_message(-1, "tc command:  \"%s\"", tc_command);
	system(tc_command);
	snprintf(tc_command, sizeof(tc_command), "%s filter del dev %s ingress >/dev/null 2>&1", tc_path, ifname);
	if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
	log_client_debug_message(-1, "tc command:  \"%s\"", tc_command);
	system(tc_command);
	snprintf(tc_command, sizeof(tc_command), "%s qdisc del dev %s clsact >/dev/null 2>&1", tc_path, ifname);
	if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
	log_client_debug_message(-1, "tc command:  \"%s\"", tc_command);
	system(tc_command);

	// Keep mqprio and any child qdiscs in place. On this igb path, deleting even
	// the child qdiscs can reset the adapter. Startup will re-use or replace the
	// existing qdisc stack as needed.
}

static int run_cmd(const char *cmd, int log_failure)
{
	int rc = system(cmd);
	if (rc == -1)
	{
		if (log_failure)
		{
			log_client_error_message(-1, "command(\"%s\") failed to execute", cmd);
		}
		return -1;
	}
	if (WIFEXITED(rc))
	{
		int status = WEXITSTATUS(rc);
		if (status != 0 && log_failure)
		{
			log_client_error_message(-1, "command(\"%s\") exited with status %d", cmd, status);
		}
		return status;
	}
	if (WIFSIGNALED(rc))
	{
		if (log_failure)
		{
			log_client_error_message(-1, "command(\"%s\") terminated by signal %d", cmd, WTERMSIG(rc));
		}
		return -1;
	}
	return 0;
}

static int has_mqprio_root(const char *ifname)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "%s qdisc show dev %s | grep -q \"mqprio 1:\"",
		tc_path, ifname);
	return run_cmd(cmd, 0) == 0;
}

static int has_parent_qdisc(const char *ifname, const char *parent)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "%s qdisc show dev %s | grep -q \"parent %s\"",
		tc_path, ifname, parent);
	return run_cmd(cmd, 0) == 0;
}

static int ensure_root_qdisc_installed(int sockfd, const char *ifname)
{
	char tc_command[1000] = {0};
	const char *mqprio_hw;
	int hw = 0;

	if (!ifname || ifname[0] == '\0' || skip_root_qdisc)
	{
		return 0;
	}

	if (root_qdisc_installed || has_mqprio_root(ifname))
	{
		root_qdisc_installed = 1;
		return 0;
	}

	mqprio_hw = getenv("SHAPER_MQPRIO_HW");
	if (mqprio_hw && atoi(mqprio_hw) > 0)
	{
		hw = 1;
	}

	snprintf(tc_command, sizeof(tc_command), "%s qdisc add dev %s root handle 1: mqprio num_tc 4 map %s queues 1@0 1@1 1@2 1@3 hw %d >/dev/null 2>&1", tc_path, ifname, shaper_tc_map, hw);
	if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
	log_client_debug_message(sockfd, "tc command:  \"%s\"", tc_command);
	if (run_cmd(tc_command, 0) != 0)
	{
		if (has_mqprio_root(ifname))
		{
			root_qdisc_installed = 1;
			return 0;
		}

		snprintf(tc_command, sizeof(tc_command), "%s qdisc replace dev %s root handle 1: mqprio num_tc 4 map %s queues 1@0 1@1 1@2 1@3 hw %d >/dev/null 2>&1", tc_path, ifname, shaper_tc_map, hw);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		log_client_debug_message(sockfd, "tc command:  \"%s\"", tc_command);
		if (run_cmd(tc_command, 1) != 0)
		{
			log_client_error_message(sockfd, "command(\"%s\") failed", tc_command);
			return -1;
		}
	}

	root_qdisc_installed = 1;
	return 0;
}

static void preinit_interface_qdisc(const char *ifname)
{
	if (!ifname || ifname[0] == '\0')
	{
		return;
	}

	if (!interface_cleaned)
	{
		cleanup_qdisc(ifname);
		interface_cleaned = 1;
	}
	if (ensure_root_qdisc_installed(-1, ifname) == 0)
	{
		strncpy(interface, ifname, sizeof(interface) - 1);
		interface[sizeof(interface) - 1] = '\0';
		apply_egress_qmap_filters(ifname);
		preseed_class_qdiscs(ifname);
		SHAPER_LOGF_INFO("Preinitialized interface qdisc state on %s", ifname);
	}
}

static void preseed_class_qdiscs(const char *ifname)
{
	if (!ifname || ifname[0] == '\0' || link_speed_mbps <= 0)
	{
		return;
	}

	apply_seed_class_qdisc(-1, ifname, SHAPER_CLASS_A);
	apply_seed_class_qdisc(-1, ifname, SHAPER_CLASS_B);
}

static void apply_seed_class_qdisc(int sockfd, const char *ifname, shaper_class_t class_type)
{
	int port_rate_bps;
	int seed_idleslope_bps = 1000000;
	int seed_frame_size = 64;
	int sendslope_bps = 0;
	int hicredit = 0;
	int locredit = 0;
	int class_bw = seed_idleslope_bps;
	int class_frame_size = seed_frame_size;
	const char *parent_str = NULL;
	int parent = 0;
	shaper_qdisc_mode_t qdisc_mode = SHAPER_QDISC_CBS;
	int cbs_offload = 0;
	long long etf_delta_ns = 0;
	int etf_offload = 0;
	int etf_skip_sock_check = 0;

	if (!ifname || ifname[0] == '\0' || link_speed_mbps <= 0)
	{
		return;
	}

	port_rate_bps = link_speed_mbps * 1000000;
	if (class_type == SHAPER_CLASS_A)
	{
		parent_str = classa_parent_str;
		parent = classa_parent;
		qdisc_mode = classa_qdisc_mode;
		cbs_offload = classa_cbs_offload;
		etf_delta_ns = classa_etf_delta_ns;
		etf_offload = classa_etf_offload;
		etf_skip_sock_check = classa_etf_skip_sock_check;
		if (classa_preset_active)
		{
			class_bw = classa_preset_bw;
			class_frame_size = classa_preset_max_frame_size;
		}
	}
	else
	{
		parent_str = classb_parent_str;
		parent = classb_parent;
		qdisc_mode = classb_qdisc_mode;
		cbs_offload = classb_cbs_offload;
		etf_delta_ns = classb_etf_delta_ns;
		etf_offload = classb_etf_offload;
		etf_skip_sock_check = classb_etf_skip_sock_check;
		if (classb_preset_active)
		{
			class_bw = classb_preset_bw;
			class_frame_size = classb_preset_max_frame_size;
		}
	}

	if (qdisc_mode == SHAPER_QDISC_ETF)
	{
		tc_etf_command(sockfd, (char *)ifname, parent_str, parent,
			etf_delta_ns, etf_offload, etf_skip_sock_check);
	}
	else if (compute_cbs_params(port_rate_bps, class_bw, class_frame_size,
			&sendslope_bps, &hicredit, &locredit) == 0)
	{
		if (qdisc_mode == SHAPER_QDISC_CBS_ETF)
		{
			tc_cbs_etf_command(sockfd, (char *)ifname, parent_str, parent,
				class_bw, sendslope_bps, hicredit, locredit, cbs_offload,
				etf_delta_ns, etf_offload, etf_skip_sock_check);
		}
		else
		{
			tc_cbs_command(sockfd, (char *)ifname, parent_str, parent,
				class_bw, sendslope_bps, hicredit, locredit, cbs_offload);
		}
	}

	if (class_type == SHAPER_CLASS_A)
	{
		if (classa_preset_active)
		{
			classa_bw = classa_preset_bw;
			classa_max_frame_size = classa_preset_max_frame_size;
			sr_classa = 1;
		}
	}
	else if (classb_preset_active)
	{
		classb_bw = classb_preset_bw;
		classb_max_frame_size = classb_preset_max_frame_size;
		sr_classb = 1;
	}
}

static int parse_parent_queue_index(const char *parent)
{
	const char *colon = NULL;
	unsigned int class_hex = 0;

	if (!parent || parent[0] == '\0')
	{
		return -1;
	}

	colon = strchr(parent, ':');
	if (!colon || *(colon + 1) == '\0')
	{
		return -1;
	}

	// tc classids are rendered in hex (e.g., 100:a), so parse as hex.
	if (sscanf(colon + 1, "%x", &class_hex) != 1 || class_hex == 0 || class_hex > 16)
	{
		return -1;
	}

	return (int)class_hex - 1;
}

static void apply_egress_qmap_filters(const char *ifname)
{
	char tc_command[1000] = {0};

	if (!ifname || ifname[0] == '\0')
	{
		return;
	}

	// Ensure clsact exists; ignore failure on add because it may already exist.
	snprintf(tc_command, sizeof(tc_command), "%s qdisc add dev %s clsact >/dev/null 2>&1", tc_path, ifname);
	if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
	run_cmd(tc_command, 0);

	// Always clear prior egress filters so stale queue mapping does not persist across runs.
	snprintf(tc_command, sizeof(tc_command), "%s filter del dev %s egress >/dev/null 2>&1", tc_path, ifname);
	if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
	run_cmd(tc_command, 0);

	if (!egress_qmap_enable)
	{
		// No explicit queue mapping requested: remove clsact to avoid lingering filter behavior.
		snprintf(tc_command, sizeof(tc_command), "%s qdisc del dev %s clsact >/dev/null 2>&1", tc_path, ifname);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		run_cmd(tc_command, 0);
		return;
	}

	// Keep gPTP (ethertype 0x88f7) on a dedicated queue when configured.
	if (qmap_gptp >= 0)
	{
		snprintf(tc_command, sizeof(tc_command),
			"%s filter add dev %s egress protocol 0x88f7 prio 5 flower action skbedit queue_mapping %d >/dev/null 2>&1",
			tc_path, ifname, qmap_gptp);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		run_cmd(tc_command, 0);
	}

	// OpenAvnu Qmgr encodes SR class in fwmark upper byte:
	// Class A => 0x0100, Class B => 0x0200 (mask 0xff00).
	if (qmap_classa >= 0)
	{
		snprintf(tc_command, sizeof(tc_command),
			"%s filter add dev %s egress protocol all prio 10 u32 match mark 0x00000100 0x0000ff00 action skbedit queue_mapping %d >/dev/null 2>&1",
			tc_path, ifname, qmap_classa);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		run_cmd(tc_command, 0);
	}
	if (qmap_classb >= 0)
	{
		snprintf(tc_command, sizeof(tc_command),
			"%s filter add dev %s egress protocol all prio 20 u32 match mark 0x00000200 0x0000ff00 action skbedit queue_mapping %d >/dev/null 2>&1",
			tc_path, ifname, qmap_classb);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		run_cmd(tc_command, 0);
	}

	if (qmap_default >= 0)
	{
		snprintf(tc_command, sizeof(tc_command),
			"%s filter add dev %s egress prio 100 matchall action skbedit queue_mapping %d >/dev/null 2>&1",
			tc_path, ifname, qmap_default);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		run_cmd(tc_command, 0);
	}
}

static const char *qdisc_mode_name(shaper_qdisc_mode_t mode)
{
	switch (mode)
	{
		case SHAPER_QDISC_ETF:
			return "etf";
		case SHAPER_QDISC_CBS_ETF:
			return "cbs_etf";
		case SHAPER_QDISC_CBS:
		default:
			return "cbs";
	}
}

static void tc_cbs_command(int sockfd, char interface[], const char *parent, int handle,
	int idleslope_bps, int sendslope_bps, int hicredit, int locredit, int offload)
{
	char tc_command[1000]={0};
	const char *verbs_replace_first[] = { "replace", "change", "add" };
	const char *verbs_add_first[] = { "add", "change", "replace" };
	const char **verbs = verbs_add_first;
	int i;
	if (has_parent_qdisc(interface, parent))
	{
		verbs = verbs_replace_first;
	}
	for (i = 0; i < 3; ++i)
	{
		snprintf(tc_command, sizeof(tc_command),
			"%s qdisc %s dev %s parent %s handle %d: cbs idleslope %d sendslope %d hicredit %d locredit %d offload %d >/dev/null 2>&1",
			tc_path, verbs[i], interface, parent, handle, idleslope_bps, sendslope_bps, hicredit, locredit, offload);
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		log_client_debug_message(sockfd, "tc command:  \"%s\"", tc_command);
		if (run_cmd(tc_command, 1) == 0)
		{
			return;
		}
	}

	// If CBS qdisc isn't available, try loading the module once and retry.
	if (!tc_cbs_checked)
	{
		tc_cbs_checked = 1;
		const char *auto_modprobe = getenv("SHAPER_AUTO_MODPROBE");
		if (auto_modprobe && atoi(auto_modprobe) > 0)
		{
			char modprobe_cmd[256];
			snprintf(modprobe_cmd, sizeof(modprobe_cmd), "%s sch_cbs", modprobe_path);
			if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", modprobe_cmd); }
			log_client_debug_message(sockfd, "tc command:  \"%s\"", modprobe_cmd);
			run_cmd(modprobe_cmd, 0);

			// Retry once
			for (i = 0; i < 3; ++i)
			{
				snprintf(tc_command, sizeof(tc_command),
					"%s qdisc %s dev %s parent %s handle %d: cbs idleslope %d sendslope %d hicredit %d locredit %d offload %d >/dev/null 2>&1",
					tc_path, verbs[i], interface, parent, handle, idleslope_bps, sendslope_bps, hicredit, locredit, offload);
				if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
				log_client_debug_message(sockfd, "tc command:  \"%s\"", tc_command);
				if (run_cmd(tc_command, 1) == 0)
				{
					return;
				}
			}
		}
	}
}

static void tc_cbs_etf_command(int sockfd, char interface[], const char *parent, int handle,
	int idleslope_bps, int sendslope_bps, int hicredit, int locredit, int cbs_offload,
	long long delta_ns, int etf_offload, int etf_skip_sock_check)
{
	char child_parent[16];
	int etf_handle = handle * 10;

	tc_cbs_command(sockfd, interface, parent, handle,
		idleslope_bps, sendslope_bps, hicredit, locredit, cbs_offload);
	snprintf(child_parent, sizeof(child_parent), "%d:1", handle);
	if (has_parent_qdisc(interface, child_parent))
	{
		// ETF settings are static for the life of the run; avoid redundant reconfigure noise.
		return;
	}
	tc_etf_command(sockfd, interface, child_parent, etf_handle,
		delta_ns, etf_offload, etf_skip_sock_check);
}

static void tc_etf_command(int sockfd, char interface[], const char *parent, int handle,
	long long delta_ns, int offload, int skip_sock_check)
{
	char tc_command[1000] = {0};
	const char *verbs[] = {"add", "replace"};
	int i;
	int appended;

	for (i = 0; i < 2; ++i)
	{
		appended = snprintf(tc_command, sizeof(tc_command),
			"%s qdisc %s dev %s parent %s handle %d: etf delta %lld clockid CLOCK_TAI",
			tc_path, verbs[i], interface, parent, handle, delta_ns);
		if (offload && appended > 0 && appended < (int)sizeof(tc_command))
		{
			appended += snprintf(tc_command + appended, sizeof(tc_command) - appended, " offload");
		}
		if (skip_sock_check && appended > 0 && appended < (int)sizeof(tc_command))
		{
			appended += snprintf(tc_command + appended, sizeof(tc_command) - appended, " skip_sock_check");
		}
		if (appended > 0 && appended < (int)sizeof(tc_command))
		{
			snprintf(tc_command + appended, sizeof(tc_command) - appended, " >/dev/null 2>&1");
		}
		if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
		log_client_debug_message(sockfd, "tc command:  \"%s\"", tc_command);
		if (run_cmd(tc_command, i == 1) == 0)
		{
			return;
		}
	}

	if (!tc_etf_checked)
	{
		tc_etf_checked = 1;
		const char *auto_modprobe = getenv("SHAPER_AUTO_MODPROBE");
		if (auto_modprobe && atoi(auto_modprobe) > 0)
		{
			char modprobe_cmd[256];
			snprintf(modprobe_cmd, sizeof(modprobe_cmd), "%s sch_etf", modprobe_path);
			if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", modprobe_cmd); }
			log_client_debug_message(sockfd, "tc command:  \"%s\"", modprobe_cmd);
			run_cmd(modprobe_cmd, 0);

			for (i = 0; i < 2; ++i)
			{
				appended = snprintf(tc_command, sizeof(tc_command),
					"%s qdisc %s dev %s parent %s handle %d: etf delta %lld clockid CLOCK_TAI",
					tc_path, verbs[i], interface, parent, handle, delta_ns);
				if (offload && appended > 0 && appended < (int)sizeof(tc_command))
				{
					appended += snprintf(tc_command + appended, sizeof(tc_command) - appended, " offload");
				}
				if (skip_sock_check && appended > 0 && appended < (int)sizeof(tc_command))
				{
					appended += snprintf(tc_command + appended, sizeof(tc_command) - appended, " skip_sock_check");
				}
				if (appended > 0 && appended < (int)sizeof(tc_command))
				{
					snprintf(tc_command + appended, sizeof(tc_command) - appended, " >/dev/null 2>&1");
				}
				if (log_tc_commands) { SHAPER_LOGF_INFO("tc: %s", tc_command); }
				log_client_debug_message(sockfd, "tc command:  \"%s\"", tc_command);
				if (run_cmd(tc_command, i == 1) == 0)
				{
					return;
				}
			}
		}
	}
}

// Returns 1 if successful, -1 on an error, or 0 if exit requested.
int process_command(int sockfd, char command[])
{
	cmd_ip input = parse_cmd(command);
	int sendslope_bps = 0, hicredit = 0, locredit = 0;
	int class_bw = 0, class_max_frame = 0;

	SHAPER_LOGF_INFO("Parsed cmd: reserve=%d unreserve=%d if=%s classA=%d classB=%d s=%d b=%d f=%d l=%d a=%s",
		input.reserve_bw, input.unreserve_bw, input.interface,
		input.class_a, input.class_b, input.measurement_interval,
		input.max_frame_size, input.max_frame_interval,
		input.link_speed_mbps, input.stream_da);

	if (input.reserve_bw && input.unreserve_bw)
	{
		log_client_error_message(sockfd, "Cannot reserve and unreserve bandwidth at the same time");
		usage(sockfd);
		return -1;
	}


	if (input.reserve_bw)
	{
		if (input.max_frame_size == 0 || input.measurement_interval == 0 || input.max_frame_interval == 0 ||
			(!input.class_a && !input.class_b))
		{
			log_client_error_message(sockfd, "class, max_frame_size, measurement_interval and max_frame_interval are mandatory inputs");
			usage(sockfd);
			return -1;
		}

		if (input.class_a && input.class_b)
		{
			log_client_error_message(sockfd, "Cannot reserve for both class A and class B");
			usage(sockfd);
			return -1;
		}
	}
	if (input.unreserve_bw)
	{
		if (input.stream_da[0] == '\0')
		{
			log_client_error_message(sockfd, "Stream Destination Address is required to unreserve bandwidth");
			usage(sockfd);
			return -1;
		}
	}
	if ((input.quit==1 || input.delete_qdisc==1) &&(input.reserve_bw==1 || input.unreserve_bw==1))
	{
		log_client_error_message(sockfd, "Quit or Delete cannot be used with other commands");
		usage(sockfd);
		return -1;
	}

	if (input.quit == 1 || input.delete_qdisc == 1)
	{
		if (sr_classa != 0 || sr_classb != 0)
		{
			//delete all the Stream DAs in list
			delete_streamda_list();
			if (strlen(interface) != 0)
			{
				cleanup_qdisc(interface);
			}
			sr_classa = sr_classb = 0;
			classa_bw = classb_bw = 0;
			classa_max_frame_size = classb_max_frame_size = 0;
		}

		if (input.quit == 1)
		{
			return 0;
		}
	}

	if (sr_classa == 0 && sr_classb == 0)
	{
		//Initializing qdisc
		if (strlen(input.interface)==0)
		{
			if (input.unreserve_bw || input.reserve_bw)
			{
				log_client_error_message(sockfd, "Interface is mandatory for the first command");
			}
			usage(sockfd);
			return -1;
		}

		if (!interface_cleaned)
		{
			cleanup_qdisc(input.interface);
			interface_cleaned = 1;
		}
		if (ensure_root_qdisc_installed(sockfd, input.interface) != 0)
		{
			return -1;
		}
		strcpy(interface,input.interface);
		apply_egress_qmap_filters(input.interface);
	}

	if (input.reserve_bw)
	{
		bandwidth_bps = (int)ceil((1/(input.measurement_interval*pow(10,-6))) * (input.max_frame_size*8) * input.max_frame_interval);
		SHAPER_LOGF_INFO("Computed stream bandwidth=%d bps (interval=%dus frame_size=%d frame_interval=%d)",
			bandwidth_bps, input.measurement_interval, input.max_frame_size, input.max_frame_interval);
	}

	// Per-command link speed (if provided) must override any startup/default value.
	if (input.reserve_bw && input.link_speed_mbps > 0)
	{
		if (link_speed_mbps != input.link_speed_mbps) {
			SHAPER_LOGF_INFO("Using command link speed: %d Mbps (previous %d Mbps)",
				input.link_speed_mbps, link_speed_mbps);
		}
		link_speed_mbps = input.link_speed_mbps;
	}

	if (input.reserve_bw == 1)
	{
		if (input.stream_da[0] == '\0')
		{
			log_client_error_message(sockfd, "Stream Destination Address is required to reserve bandwidth");
			usage(sockfd);
			return -1;
		}
		if (check_stream_da(sockfd, input.stream_da))
		{
			return 1;
		}
		if (input.class_a)
		{
			if (input.measurement_interval != 125 && input.measurement_interval != 136)
			{
				log_client_error_message(sockfd, "Measurement Interval (%d) doesn't match that of Class A (125 or 136) traffic. "
						"Enter a valid measurement interval",
						input.measurement_interval);
				return -1;
			}
			insert_stream_da(sockfd, input.stream_da, bandwidth_bps, input.max_frame_size, SHAPER_CLASS_A);

			if (link_speed_mbps == 0)
			{
				link_speed_mbps = input.link_speed_mbps;
			}
			if (link_speed_mbps == 0)
			{
				log_client_error_message(sockfd, "Link speed is required for CBS shaping. Use -l <Mbps>");
				return -1;
			}

			if (class_preset_enabled(SHAPER_CLASS_A))
			{
				SHAPER_LOGF_INFO("Class A preset active; keeping qdisc at bw=%d max_frame=%d after reserve for %s",
					classa_bw, classa_max_frame_size, input.stream_da);
			}
			else
			{
				classa_bw += bandwidth_bps;
				if (input.max_frame_size > classa_max_frame_size)
				{
					classa_max_frame_size = input.max_frame_size;
				}

				if (classa_qdisc_mode == SHAPER_QDISC_ETF)
				{
					tc_etf_command(sockfd, interface, classa_parent_str, classa_parent,
						classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
				}
				else if (classa_qdisc_mode == SHAPER_QDISC_CBS_ETF)
				{
					if (compute_cbs_params(link_speed_mbps * 1000000, classa_bw, classa_max_frame_size,
						&sendslope_bps, &hicredit, &locredit) < 0)
					{
						SHAPER_LOGF_ERROR("CBS compute failed A: port_rate=%d idleslope=%d max_frame=%d",
							link_speed_mbps * 1000000, classa_bw, classa_max_frame_size);
						log_client_error_message(sockfd, "Invalid CBS parameters for Class A. Check link speed and bandwidth");
						return -1;
					}
					tc_cbs_etf_command(sockfd, interface, classa_parent_str, classa_parent,
						classa_bw, sendslope_bps, hicredit, locredit, classa_cbs_offload,
						classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
				}
				else
				{
					if (compute_cbs_params(link_speed_mbps * 1000000, classa_bw, classa_max_frame_size,
						&sendslope_bps, &hicredit, &locredit) < 0)
					{
						SHAPER_LOGF_ERROR("CBS compute failed A: port_rate=%d idleslope=%d max_frame=%d",
							link_speed_mbps * 1000000, classa_bw, classa_max_frame_size);
						log_client_error_message(sockfd, "Invalid CBS parameters for Class A. Check link speed and bandwidth");
						return -1;
					}
					tc_cbs_command(sockfd, interface, classa_parent_str, classa_parent,
						classa_bw, sendslope_bps, hicredit, locredit, classa_cbs_offload);
				}
			}
			sr_classa = 1;
		}
		else
		{
			if (input.measurement_interval != 250 && input.measurement_interval != 272)
			{
				log_client_error_message(sockfd, "Measurement Interval (%d) doesn't match that of Class B (250 or 272) traffic. "
						"Enter a valid measurement interval",
						input.measurement_interval);
				return -1;
			}
			insert_stream_da(sockfd, input.stream_da, bandwidth_bps, input.max_frame_size, SHAPER_CLASS_B);

			if (link_speed_mbps == 0)
			{
				link_speed_mbps = input.link_speed_mbps;
			}
			if (link_speed_mbps == 0)
			{
				log_client_error_message(sockfd, "Link speed is required for CBS shaping. Use -l <Mbps>");
				return -1;
			}

			if (class_preset_enabled(SHAPER_CLASS_B))
			{
				SHAPER_LOGF_INFO("Class B preset active; keeping qdisc at bw=%d max_frame=%d after reserve for %s",
					classb_bw, classb_max_frame_size, input.stream_da);
			}
			else
			{
				classb_bw += bandwidth_bps;
				if (input.max_frame_size > classb_max_frame_size)
				{
					classb_max_frame_size = input.max_frame_size;
				}

				if (classb_qdisc_mode == SHAPER_QDISC_ETF)
				{
					tc_etf_command(sockfd, interface, classb_parent_str, classb_parent,
						classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
				}
				else
				{
					if (compute_cbs_params(link_speed_mbps * 1000000, classb_bw, classb_max_frame_size,
						&sendslope_bps, &hicredit, &locredit) < 0)
					{
						SHAPER_LOGF_ERROR("CBS compute failed B: port_rate=%d idleslope=%d max_frame=%d",
							link_speed_mbps * 1000000, classb_bw, classb_max_frame_size);
						log_client_error_message(sockfd, "Invalid CBS parameters for Class B. Check link speed and bandwidth");
						return -1;
					}
					if (classb_qdisc_mode == SHAPER_QDISC_CBS_ETF)
					{
						tc_cbs_etf_command(sockfd, interface, classb_parent_str, classb_parent,
							classb_bw, sendslope_bps, hicredit, locredit, classb_cbs_offload,
							classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
					}
					else
					{
						tc_cbs_command(sockfd, interface, classb_parent_str, classb_parent,
							classb_bw, sendslope_bps, hicredit, locredit, classb_cbs_offload);
					}
				}
			}
			sr_classb = 1;
		}
	}
	else if (input.unreserve_bw==1)
	{
		stream_da *remove_stream = get_stream_da(sockfd, input.stream_da);
		if (remove_stream != NULL)
		{
			shaper_class_t class_type = remove_stream->class_type;
			char removed_dest[STREAMDA_LENGTH];
			strncpy(removed_dest, remove_stream->dest_addr, sizeof(removed_dest) - 1);
			removed_dest[sizeof(removed_dest) - 1] = '\0';
			remove_stream_da(sockfd, remove_stream->dest_addr);

			if (class_type == SHAPER_CLASS_A)
			{
				if (class_preset_enabled(SHAPER_CLASS_A))
				{
					SHAPER_LOGF_INFO("Class A preset active; keeping qdisc at bw=%d max_frame=%d after unreserve for %s",
						classa_bw, classa_max_frame_size, removed_dest);
				}
				else
				{
					recompute_class_totals(SHAPER_CLASS_A, &class_bw, &class_max_frame);
					classa_bw = class_bw;
					classa_max_frame_size = class_max_frame;

					if (classa_bw == 0)
					{
						apply_seed_class_qdisc(sockfd, interface, SHAPER_CLASS_A);
						sr_classa = 0;
					}
					else
					{
						if (classa_qdisc_mode == SHAPER_QDISC_ETF)
						{
							tc_etf_command(sockfd, interface, classa_parent_str, classa_parent,
								classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
						}
						else if (classa_qdisc_mode == SHAPER_QDISC_CBS_ETF)
						{
							if (compute_cbs_params(link_speed_mbps * 1000000, classa_bw, classa_max_frame_size,
								&sendslope_bps, &hicredit, &locredit) < 0)
							{
								SHAPER_LOGF_ERROR("CBS recompute failed A: port_rate=%d idleslope=%d max_frame=%d",
									link_speed_mbps * 1000000, classa_bw, classa_max_frame_size);
								log_client_error_message(sockfd, "Invalid CBS parameters for Class A after unreserve");
								return -1;
							}
							tc_cbs_etf_command(sockfd, interface, classa_parent_str, classa_parent,
								classa_bw, sendslope_bps, hicredit, locredit, classa_cbs_offload,
								classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
						}
						else
						{
							if (compute_cbs_params(link_speed_mbps * 1000000, classa_bw, classa_max_frame_size,
								&sendslope_bps, &hicredit, &locredit) < 0)
							{
								SHAPER_LOGF_ERROR("CBS recompute failed A: port_rate=%d idleslope=%d max_frame=%d",
									link_speed_mbps * 1000000, classa_bw, classa_max_frame_size);
								log_client_error_message(sockfd, "Invalid CBS parameters for Class A after unreserve");
								return -1;
							}
							tc_cbs_command(sockfd, interface, classa_parent_str, classa_parent,
								classa_bw, sendslope_bps, hicredit, locredit, classa_cbs_offload);
						}
					}
				}
			}
			else if (class_type == SHAPER_CLASS_B)
			{
				if (class_preset_enabled(SHAPER_CLASS_B))
				{
					SHAPER_LOGF_INFO("Class B preset active; keeping qdisc at bw=%d max_frame=%d after unreserve for %s",
						classb_bw, classb_max_frame_size, removed_dest);
				}
				else
				{
					recompute_class_totals(SHAPER_CLASS_B, &class_bw, &class_max_frame);
					classb_bw = class_bw;
					classb_max_frame_size = class_max_frame;

					if (classb_bw == 0)
					{
						apply_seed_class_qdisc(sockfd, interface, SHAPER_CLASS_B);
						sr_classb = 0;
					}
					else if (classb_qdisc_mode == SHAPER_QDISC_ETF)
					{
						tc_etf_command(sockfd, interface, classb_parent_str, classb_parent,
							classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
					}
					else
					{
						if (compute_cbs_params(link_speed_mbps * 1000000, classb_bw, classb_max_frame_size,
							&sendslope_bps, &hicredit, &locredit) < 0)
						{
							SHAPER_LOGF_ERROR("CBS recompute failed B: port_rate=%d idleslope=%d max_frame=%d",
								link_speed_mbps * 1000000, classb_bw, classb_max_frame_size);
							log_client_error_message(sockfd, "Invalid CBS parameters for Class B after unreserve");
							return -1;
						}
						if (classb_qdisc_mode == SHAPER_QDISC_CBS_ETF)
						{
							tc_cbs_etf_command(sockfd, interface, classb_parent_str, classb_parent,
								classb_bw, sendslope_bps, hicredit, locredit, classb_cbs_offload,
								classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
						}
						else
						{
							tc_cbs_command(sockfd, interface, classb_parent_str, classb_parent,
								classb_bw, sendslope_bps, hicredit, locredit, classb_cbs_offload);
						}
					}
				}
			}
		}
	}

	return 1;
}

int init_socket()
{
	int socketfd = 0;
	struct sockaddr_in serv_addr;
	int yes=1;
	if ((socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1 )
	{
		SHAPER_LOGF_ERROR("Could not open socket %d.  Error %d (%s)", socketfd, errno, strerror(errno));
		return -1;
	}
	if (fcntl(socketfd, F_SETFL, O_NONBLOCK) < 0)
	{
		SHAPER_LOGF_ERROR("Could not set the socket to non-blocking.  Error %d (%s)", errno, strerror(errno));
		close(socketfd);
		return -1;
	}


	memset(&serv_addr, '0', sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	serv_addr.sin_port = htons(SHAPER_PORT);
	setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

	if (bind(socketfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr))<0)
	{
		SHAPER_LOGF_ERROR("bind() error %d (%s)", errno, strerror(errno));
		return -1;
	}
	if (listen(socketfd, 10)<0)
	{
		SHAPER_LOGF_ERROR("listen() error %d (%s)", errno, strerror(errno));
		return -1;
	}

	return socketfd;

}

int main (int argc, char *argv[])
{
	char command[256];
	char stdinCommandBuf[COMMAND_BUFFER_SIZE] = {0};
	int stdinCommandLen = 0;
	char clientCommandBuf[MAX_CLIENT_CONNECTIONS][COMMAND_BUFFER_SIZE];
	int clientCommandLen[MAX_CLIENT_CONNECTIONS];
	int socketfd = 0,newfd = 0;
	int clientfd[MAX_CLIENT_CONNECTIONS];
	int i, nextclientindex;
	fd_set read_fds;
	int fdmax;
	int recvbytes;

	shaperLogInit();

	{
		const char *env_tc_path = getenv("SHAPER_TC_PATH");
		if (env_tc_path && env_tc_path[0] != '\0')
		{
			strncpy(tc_path, env_tc_path, sizeof(tc_path) - 1);
			tc_path[sizeof(tc_path) - 1] = '\0';
		}
		else if (access("/sbin/tc", X_OK) == 0)
		{
			strncpy(tc_path, "/sbin/tc", sizeof(tc_path) - 1);
			tc_path[sizeof(tc_path) - 1] = '\0';
		}
		else if (access("/usr/sbin/tc", X_OK) == 0)
		{
			strncpy(tc_path, "/usr/sbin/tc", sizeof(tc_path) - 1);
			tc_path[sizeof(tc_path) - 1] = '\0';
		}
		if (access("/sbin/modprobe", X_OK) == 0)
		{
			strncpy(modprobe_path, "/sbin/modprobe", sizeof(modprobe_path) - 1);
			modprobe_path[sizeof(modprobe_path) - 1] = '\0';
		}
		else if (access("/usr/sbin/modprobe", X_OK) == 0)
		{
			strncpy(modprobe_path, "/usr/sbin/modprobe", sizeof(modprobe_path) - 1);
			modprobe_path[sizeof(modprobe_path) - 1] = '\0';
		}

		// Ensure PATH contains sbin for system() commands.
		setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin", 1);
		{
			const char *env_log_tc = getenv("SHAPER_TC_LOG");
			if (env_log_tc && atoi(env_log_tc) > 0)
			{
				log_tc_commands = 1;
			}
		}
		SHAPER_LOGF_INFO("Using tc at: %s", tc_path);
		{
			char tc_version[256];
			snprintf(tc_version, sizeof(tc_version), "%s -V", tc_path);
			system(tc_version);
		}

		const char *skip_root = getenv("SHAPER_SKIP_ROOT_QDISC");
		if (skip_root && atoi(skip_root) > 0)
		{
			skip_root_qdisc = 1;
		}
			const char *env_link_speed = getenv("SHAPER_LINK_SPEED_MBPS");
			if (env_link_speed)
			{
				link_speed_mbps = atoi(env_link_speed);
				if (link_speed_mbps > 0) {
					SHAPER_LOGF_INFO("Initial link speed from SHAPER_LINK_SPEED_MBPS: %d Mbps", link_speed_mbps);
				}
			}
		const char *env_classa_parent = getenv("SHAPER_CLASSA_PARENT");
		const char *env_classb_parent = getenv("SHAPER_CLASSB_PARENT");
		const char *env_classa_handle = getenv("SHAPER_CLASSA_HANDLE");
		const char *env_classb_handle = getenv("SHAPER_CLASSB_HANDLE");
		const char *env_egress_qmap = getenv("SHAPER_EGRESS_QMAP");
		const char *env_classa_qmap = getenv("SHAPER_CLASSA_QMAP");
		const char *env_classb_qmap = getenv("SHAPER_CLASSB_QMAP");
		const char *env_default_qmap = getenv("SHAPER_DEFAULT_QMAP");
		const char *env_gptp_qmap = getenv("SHAPER_GPTP_QMAP");
		const char *env_classa_qdisc = getenv("SHAPER_CLASSA_QDISC");
		const char *env_classb_qdisc = getenv("SHAPER_CLASSB_QDISC");
		const char *env_classa_cbs_offload = getenv("SHAPER_CLASSA_CBS_OFFLOAD");
		const char *env_classa_etf_delta = getenv("SHAPER_CLASSA_ETF_DELTA_NS");
		const char *env_classa_etf_offload = getenv("SHAPER_CLASSA_ETF_OFFLOAD");
		const char *env_classa_etf_skip_sock_check = getenv("SHAPER_CLASSA_ETF_SKIP_SOCK_CHECK");
		const char *env_classb_cbs_offload = getenv("SHAPER_CLASSB_CBS_OFFLOAD");
		const char *env_classb_etf_delta = getenv("SHAPER_CLASSB_ETF_DELTA_NS");
		const char *env_classb_etf_offload = getenv("SHAPER_CLASSB_ETF_OFFLOAD");
		const char *env_classb_etf_skip_sock_check = getenv("SHAPER_CLASSB_ETF_SKIP_SOCK_CHECK");
		const char *env_tc_map = getenv("SHAPER_TC_MAP");
		const char *env_preset_classa_bw = getenv("SHAPER_PRESET_CLASSA_BW");
		const char *env_preset_classa_max_frame = getenv("SHAPER_PRESET_CLASSA_MAX_FRAME");
		const char *env_preset_classb_bw = getenv("SHAPER_PRESET_CLASSB_BW");
		const char *env_preset_classb_max_frame = getenv("SHAPER_PRESET_CLASSB_MAX_FRAME");
		if (env_classa_parent)
		{
			strncpy(classa_parent_str, env_classa_parent, sizeof(classa_parent_str) - 1);
			classa_parent_str[sizeof(classa_parent_str) - 1] = '\0';
		}
		if (env_classb_parent)
		{
			strncpy(classb_parent_str, env_classb_parent, sizeof(classb_parent_str) - 1);
			classb_parent_str[sizeof(classb_parent_str) - 1] = '\0';
		}
		if (env_classa_handle)
		{
			classa_parent = atoi(env_classa_handle);
		}
		if (env_classb_handle)
		{
			classb_parent = atoi(env_classb_handle);
		}
		if (env_egress_qmap && atoi(env_egress_qmap) > 0)
		{
			egress_qmap_enable = 1;
		}
		if (env_classa_qmap)
		{
			qmap_classa = atoi(env_classa_qmap);
		}
		if (env_classb_qmap)
		{
			qmap_classb = atoi(env_classb_qmap);
		}
		if (env_default_qmap)
		{
			qmap_default = atoi(env_default_qmap);
		}
		if (env_gptp_qmap)
		{
			qmap_gptp = atoi(env_gptp_qmap);
		}
		if (env_classa_qdisc)
		{
			if (!strcasecmp(env_classa_qdisc, "etf"))
			{
				classa_qdisc_mode = SHAPER_QDISC_ETF;
			}
			else if (!strcasecmp(env_classa_qdisc, "cbs_etf") || !strcasecmp(env_classa_qdisc, "cbs+etf"))
			{
				classa_qdisc_mode = SHAPER_QDISC_CBS_ETF;
			}
			else if (!strcasecmp(env_classa_qdisc, "cbs"))
			{
				classa_qdisc_mode = SHAPER_QDISC_CBS;
			}
			else
			{
				SHAPER_LOGF_WARNING("Ignoring unsupported SHAPER_CLASSA_QDISC=%s", env_classa_qdisc);
			}
		}
		if (env_classb_qdisc)
		{
			if (!strcasecmp(env_classb_qdisc, "etf"))
			{
				classb_qdisc_mode = SHAPER_QDISC_ETF;
			}
			else if (!strcasecmp(env_classb_qdisc, "cbs_etf") || !strcasecmp(env_classb_qdisc, "cbs+etf"))
			{
				classb_qdisc_mode = SHAPER_QDISC_CBS_ETF;
			}
			else if (!strcasecmp(env_classb_qdisc, "cbs"))
			{
				classb_qdisc_mode = SHAPER_QDISC_CBS;
			}
			else
			{
				SHAPER_LOGF_WARNING("Ignoring unsupported SHAPER_CLASSB_QDISC=%s", env_classb_qdisc);
			}
		}
		if (env_classa_cbs_offload)
		{
			classa_cbs_offload = atoi(env_classa_cbs_offload) > 0 ? 1 : 0;
		}
		if (env_classa_etf_delta)
		{
			classa_etf_delta_ns = atoll(env_classa_etf_delta);
		}
		if (env_classa_etf_offload)
		{
			classa_etf_offload = atoi(env_classa_etf_offload) > 0 ? 1 : 0;
		}
		if (env_classa_etf_skip_sock_check)
		{
			classa_etf_skip_sock_check = atoi(env_classa_etf_skip_sock_check) > 0 ? 1 : 0;
		}
		if (env_classb_cbs_offload)
		{
			classb_cbs_offload = atoi(env_classb_cbs_offload) > 0 ? 1 : 0;
		}
		if (env_classb_etf_delta)
		{
			classb_etf_delta_ns = atoll(env_classb_etf_delta);
		}
		if (env_classb_etf_offload)
		{
			classb_etf_offload = atoi(env_classb_etf_offload) > 0 ? 1 : 0;
		}
		if (env_classb_etf_skip_sock_check)
		{
			classb_etf_skip_sock_check = atoi(env_classb_etf_skip_sock_check) > 0 ? 1 : 0;
		}
		if (env_tc_map && *env_tc_map)
		{
			strncpy(shaper_tc_map, env_tc_map, sizeof(shaper_tc_map) - 1);
			shaper_tc_map[sizeof(shaper_tc_map) - 1] = '\0';
		}
		if (env_preset_classa_bw && env_preset_classa_max_frame)
		{
			classa_preset_bw = atoi(env_preset_classa_bw);
			classa_preset_max_frame_size = atoi(env_preset_classa_max_frame);
			if (classa_preset_bw > 0 && classa_preset_max_frame_size > 0)
			{
				classa_preset_active = 1;
				classa_bw = classa_preset_bw;
				classa_max_frame_size = classa_preset_max_frame_size;
			}
		}
		if (env_preset_classb_bw && env_preset_classb_max_frame)
		{
			classb_preset_bw = atoi(env_preset_classb_bw);
			classb_preset_max_frame_size = atoi(env_preset_classb_max_frame);
			if (classb_preset_bw > 0 && classb_preset_max_frame_size > 0)
			{
				classb_preset_active = 1;
				classb_bw = classb_preset_bw;
				classb_max_frame_size = classb_preset_max_frame_size;
			}
		}
		const char *env_init_if = getenv("SHAPER_INIT_IFACE");
		SHAPER_LOGF_INFO("Class A qdisc mode: %s", qdisc_mode_name(classa_qdisc_mode));
		SHAPER_LOGF_INFO("Class B qdisc mode: %s", qdisc_mode_name(classb_qdisc_mode));
		SHAPER_LOGF_INFO("mqprio traffic-class map: %s", shaper_tc_map);
		if (classa_preset_active)
		{
			SHAPER_LOGF_INFO("Class A preset shaping: bw=%d max_frame=%d",
				classa_preset_bw, classa_preset_max_frame_size);
		}
		if (classb_preset_active)
		{
			SHAPER_LOGF_INFO("Class B preset shaping: bw=%d max_frame=%d",
				classb_preset_bw, classb_preset_max_frame_size);
		}
		if (classa_qdisc_mode == SHAPER_QDISC_CBS || classa_qdisc_mode == SHAPER_QDISC_CBS_ETF)
		{
			SHAPER_LOGF_INFO("Class A CBS settings: offload=%d", classa_cbs_offload);
		}
		if (classa_qdisc_mode == SHAPER_QDISC_ETF || classa_qdisc_mode == SHAPER_QDISC_CBS_ETF)
		{
			SHAPER_LOGF_INFO("Class A ETF settings: delta=%lld offload=%d skip_sock_check=%d",
				classa_etf_delta_ns, classa_etf_offload, classa_etf_skip_sock_check);
		}
		if (classb_qdisc_mode == SHAPER_QDISC_CBS || classb_qdisc_mode == SHAPER_QDISC_CBS_ETF)
		{
			SHAPER_LOGF_INFO("Class B CBS settings: offload=%d", classb_cbs_offload);
		}
		if (classb_qdisc_mode == SHAPER_QDISC_ETF || classb_qdisc_mode == SHAPER_QDISC_CBS_ETF)
		{
			SHAPER_LOGF_INFO("Class B ETF settings: delta=%lld offload=%d skip_sock_check=%d",
				classb_etf_delta_ns, classb_etf_offload, classb_etf_skip_sock_check);
		}
		if (egress_qmap_enable)
		{
			if (qmap_classa < 0)
			{
				qmap_classa = parse_parent_queue_index(classa_parent_str);
			}
			if (qmap_classb < 0)
			{
				qmap_classb = parse_parent_queue_index(classb_parent_str);
			}
			SHAPER_LOGF_INFO("Egress qmap enabled: classA=%d classB=%d default=%d gPTP=%d",
				qmap_classa, qmap_classb, qmap_default, qmap_gptp);
		}
		if (env_init_if && *env_init_if)
		{
			preinit_interface_qdisc(env_init_if);
		}
	}

	while((c = getopt(argc,argv,"d"))>=0)
	{
		switch(c)
		{
			case 'd':
			daemonize = 1;
			break;
		}
	}

	if (daemonize == 1 && daemon(1,0) < 0)
	{
		SHAPER_LOGF_ERROR("Error %d (%s) starting the daemon", errno, strerror(errno));
		shaperLogExit();
		return 1;
	}

	if ((socketfd = init_socket())<0)
	{
		shaperLogExit();
		return 1;
	}

	// Setup signal handler
	// We catch SIGINT and shutdown cleanly
	int err;
	struct sigaction sa;
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	err = sigaction(SIGINT, &sa, NULL);
	if (err)
	{
		SHAPER_LOG_ERROR("Failed to setup SIGINT handler");
		shaperLogExit();
		return 1;
	}
	err = sigaction(SIGTERM, &sa, NULL);
	if (err)
	{
		SHAPER_LOG_ERROR("Failed to setup SIGTERM handler");
		shaperLogExit();
		return 1;
	}
	err = sigaction(SIGUSR1, &sa, NULL);
	if (err)
	{
		SHAPER_LOG_ERROR("Failed to setup SIGUSR1 handler");
		shaperLogExit();
		return 1;
	}

	// Ignore SIGPIPE signals.
	signal(SIGPIPE, SIG_IGN);

	for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i)
	{
		clientfd[i] = -1;
		clientCommandLen[i] = 0;
		clientCommandBuf[i][0] = '\0';
	}
	nextclientindex = 0;

	fputs(USER_COMMAND_PROMPT, stdout);
	fflush(stdout);

	while (!exit_received)
	{
		FD_ZERO(&read_fds);
		if (!daemonize)
		{
			FD_SET(STDIN_FILENO, &read_fds);
		}
		FD_SET(socketfd, &read_fds);
		fdmax = socketfd;
		for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i)
		{
			if (clientfd[i] > 0)
			{
				FD_SET(clientfd[i], &read_fds);
				if (clientfd[i] > fdmax)
				{
					fdmax = clientfd[i];
				}
			}
		}
		int n = select(fdmax+1, &read_fds, NULL, NULL, NULL);
		if(n == -1)
		{
			if (exit_received)
			{
				// Assume the app received a signal to quit.
				// Process the quit command.
				process_command(-1, "-q");
			}
			else
			{
				SHAPER_LOGF_ERROR("select() error %d (%s)", errno, strerror(errno));
				break;
			}
		}
		else
		{
			/* Handle any commands received via stdin. */
			if (FD_ISSET(STDIN_FILENO, &read_fds))
			{
			recvbytes = read(STDIN_FILENO, command, sizeof(command) - 1);
			if (recvbytes < 0)
			{
				SHAPER_LOGF_ERROR("Error %d reading from stdin (%s)", errno, strerror(errno));
			}
			else if (recvbytes == 0)
			{
				// EOF on stdin; exit cleanly.
				process_command(-1, "-q");
				exit_received = 1;
			}
			else
			{
				/* Process the command data (may include multiple lines). */
					int ret = process_commands_from_stream(-1, stdinCommandBuf, &stdinCommandLen, command, recvbytes);
				if (!ret)
				{
					/* Received a command to exit. */
					exit_received = 1;
				}
					else
					{
						/* Prompt the user again. */
						shaperLogDisplayAll();
						fputs(USER_COMMAND_PROMPT, stdout);
						fflush(stdout);
					}
				}
			}

			if (FD_ISSET(socketfd, &read_fds))
			{
				newfd = accept(socketfd, (struct sockaddr*)NULL, NULL);
				if (clientfd[nextclientindex] != -1)
				{
					/* Find the next available index. */
					for (i = (nextclientindex + 1) % MAX_CLIENT_CONNECTIONS; i != nextclientindex; i = (i + 1) % MAX_CLIENT_CONNECTIONS)
					{
						if (clientfd[nextclientindex] == -1)
						{
							/* Found an empty array slot. */
							break;
						}
					}
					if (i == nextclientindex)
					{
						/* No more client slots available.  Connection rejected. */
						SHAPER_LOG_WARNING("Out of client connection slots.  Connection rejected.");
						close(newfd);
						newfd = -1;
					}
				}


					if (newfd != -1)
					{
						clientfd[nextclientindex] = newfd;
						clientCommandLen[nextclientindex] = 0;
						clientCommandBuf[nextclientindex][0] = '\0';
						nextclientindex = (nextclientindex + 1) % MAX_CLIENT_CONNECTIONS; /* Next slot used for the next try. */

					/* Send a prompt to the user. */
					send(newfd, USER_COMMAND_PROMPT, strlen(USER_COMMAND_PROMPT), 0);
				}
			}

			for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i)
			{
				if (clientfd[i] != -1 && FD_ISSET(clientfd[i], &read_fds))
				{
					recvbytes = recv(clientfd[i], command, sizeof(command) - 1, 0);
						if (recvbytes < 0)
						{
							SHAPER_LOGF_ERROR("Error %d reading from socket %d (%s).  Connection closed.", errno, clientfd[i], strerror(errno));
								cleanup_client_streams(clientfd[i]);
								close(clientfd[i]);
								clientfd[i] = -1;
								clientCommandLen[i] = 0;
							clientCommandBuf[i][0] = '\0';
							nextclientindex = i; /* We know this slot will be empty. */
							continue;
						}
						if (recvbytes == 0)
						{
							SHAPER_LOGF_INFO("Socket %d closed", clientfd[i]);
							cleanup_client_streams(clientfd[i]);
							close(clientfd[i]);
							clientfd[i] = -1;
							clientCommandLen[i] = 0;
							clientCommandBuf[i][0] = '\0';
							nextclientindex = i; /* We know this slot will be empty. */
							continue;
						}

						/* Process the command data (may include multiple lines). */
						int ret = process_commands_from_stream(
							clientfd[i], clientCommandBuf[i], &clientCommandLen[i], command, recvbytes);
					if (!ret)
					{
						/* Received a command to exit. */
						exit_received = 1;
					}
					else
					{
						/* Send another prompt to the user. */
						send(clientfd[i], USER_COMMAND_PROMPT, strlen(USER_COMMAND_PROMPT), 0);
					}
				}
			}
		}
	}//main while loop

	close(socketfd);

	/* Close any connected sockets. */
	for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i) {
		if (clientfd[i] != -1) {
			SHAPER_LOGF_INFO("Socket %d closed", clientfd[i]);
			cleanup_client_streams(clientfd[i]);
			close(clientfd[i]);
			clientfd[i] = -1;
		}
	}

	shaperLogExit();

	if (interface[0] != '\0')
	{
		cleanup_qdisc(interface);
	}

	return 0;
}
