/*
 *  This file is part of carbon-c-relay.
 *
 *  carbon-c-relay is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  carbon-c-relay is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with carbon-c-relay.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "relay.h"
#include "carbon-hash.h"
#include "server.h"
#include "router.h"
#include "receptor.h"
#include "dispatcher.h"
#include "collector.h"

int keep_running = 1;
char relay_hostname[128];

static void
exit_handler(int sig)
{
	char *signal = "unknown signal";

	switch (sig) {
		case SIGTERM:
			signal = "SIGTERM";
			break;
		case SIGINT:
			signal = "SIGINT";
			break;
		case SIGQUIT:
			signal = "SIGQUIT";
			break;
	}
	fprintf(stdout, "caught %s, terminating...\n", signal);
	keep_running = 0;
}

void
do_version(void)
{
	printf("carbon-c-relay v" VERSION " (" GIT_VERSION ")\n");

	exit(0);
}

void
do_usage(int exitcode)
{
	printf("Usage: relay [-v] -f <config> [-p <port>] [-w <workers>]\n");
	printf("\n");
	printf("Options:\n");
	printf("  -v  print version and exit\n");
	printf("  -f  read <config> for clusters and routes\n");
	printf("  -p  listen on <port> for connections, defaults to 2003\n");
	printf("  -w  user <workers> worker threads, defaults to 16\n");

	exit(exitcode);
}

int
main(int argc, char * const argv[])
{
	int sock;
	char id;
	server **servers;
	dispatcher **workers;
	char workercnt = 16;
	char *routes = NULL;
	unsigned short listenport = 2003;
	int bflag, ch;

	bflag = 0;
	while ((ch = getopt(argc, argv, ":hvf:p:w:")) != -1) {
		switch (ch) {
			case 'v':
				do_version();
				break;
			case 'f':
				routes = optarg;
				break;
			case 'p':
				listenport = (unsigned short)atoi(optarg);
				if (listenport == 0) {
					fprintf(stderr, "error: port needs to be a number >0\n");
					do_usage(1);
				}
				break;
			case 'w':
				workercnt = (char)atoi(optarg);
				if (workercnt <= 0) {
					fprintf(stderr, "error: workers needs to be a number >0\n");
					do_usage(1);
				}
				break;
			case '?':
			case ':':
				do_usage(1);
				break;
			case 'h':
			default:
				do_usage(0);
				break;
		}
	}
	if (optind == 1)
		do_usage(1);


	if (gethostname(relay_hostname, sizeof(relay_hostname)) < 0)
		snprintf(relay_hostname, sizeof(relay_hostname), "127.0.0.1");

	fprintf(stdout, "Starting carbon-c-relay v%s (%s)\n",
		VERSION, GIT_VERSION);
	fprintf(stdout, "configuration:\n");
	fprintf(stdout, "    relay hostname = %s\n", relay_hostname);
	fprintf(stdout, "    listen port = %u\n", listenport);
	fprintf(stdout, "    workers = %d\n", workercnt);
	fprintf(stdout, "    routes configuration = %s\n", routes);
	fprintf(stdout, "\n");
	if (router_readconfig(routes) == 0) {
		fprintf(stderr, "failed to read configuration '%s'\n", routes);
		return 1;
	}
	fprintf(stdout, "parsed configuration follows:\n");
	router_printconfig(stdout);
	fprintf(stdout, "\n");

	if (signal(SIGINT, exit_handler) == SIG_ERR) {
		fprintf(stderr, "failed to create SIGINT handler: %s\n",
				strerror(errno));
		return 1;
	}
	if (signal(SIGTERM, exit_handler) == SIG_ERR) {
		fprintf(stderr, "failed to create SIGTERM handler: %s\n",
				strerror(errno));
		return 1;
	}
	if (signal(SIGQUIT, exit_handler) == SIG_ERR) {
		fprintf(stderr, "failed to create SIGQUIT handler: %s\n",
				strerror(errno));
		return 1;
	}
	workers = malloc(sizeof(dispatcher *) * (workercnt + 1));
	if (workers == NULL) {
		fprintf(stderr, "failed to allocate memory for workers\n");
		return 1;
	}

	sock = bindlisten(listenport);
	if (sock < 0) {
		fprintf(stderr, "failed to bind on port %d: %s\n",
				listenport, strerror(errno));
		return -1;
	}
	if (dispatch_addlistener(sock) != 0) {
		close(sock);
		fprintf(stderr, "failed to add listener\n");
		return -1;
	}
	fprintf(stdout, "listening on port %u\n", listenport);

	fprintf(stderr, "starting %d workers\n", workercnt);
	for (id = 1; id <= workercnt; id++) {
		workers[id - 1] = dispatch_new(id);
		if (workers[id - 1] == NULL) {
			fprintf(stderr, "failed to add worker %d\n", id);
			break;
		}
	}
	workers[id - 1] = NULL;
	if (id <= workercnt) {
		fprintf(stderr, "shutting down due to errors\n");
		keep_running = 0;
	}

	servers = router_getservers();
	collector_start((void **)workers, (void **)servers);

	/* workers do the work, just wait */
	while (keep_running)
		sleep(1);

	fprintf(stdout, "shutting down...\n");
	router_shutdown();
	/* since workers will be freed, stop querying the structures */
	collector_stop();
	for (id = 0; id < workercnt; id++)
		dispatch_shutdown(workers[id + 0]);
	fprintf(stdout, "%d workers stopped\n", workercnt);

	free(workers);
	return 0;
}
