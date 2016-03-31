#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>

#include "postgres.h"
#include "postmaster/bgworker.h"

#include "raft.h"
#include "util.h"

#include "state.h"
#include "worker.h"

#define MAX_CLIENTS 1024
#define LISTEN_QUEUE_SIZE 10
#define BUFLEN 1024

typedef struct Expectation {
	int id;
	int index;
} Expectation;

typedef struct Client {
	bool good;
	int sock;
	char buf[BUFLEN];
	size_t bufrecved;
	char *msg;
	size_t msgrecved;
	size_t msglen;
	Expectation expect;
} Client;

typedef struct Server {
	char *host;
	int port;

	int listener;
	int raftsock;

	fd_set all;
	int maxfd;
	int clientnum;
	Client clients[MAX_CLIENTS];
} Server;

static Server server;
static raft_t raft;

static void applier(void *state, raft_update_t update, raft_bool_t snapshot)
{
	Assert(state);
	state_update(state, (RaftableUpdate *)update.data, snapshot);
}

static raft_update_t snapshooter(void *state)
{
	Assert(state);
	raft_update_t shot;
	size_t shotlen;
	shot.data = state_make_snapshot(state, &shotlen);
	shot.len = shotlen;
	return shot;
}

static void add_peers(WorkerConfig *cfg)
{
	int i;
	for (i = 0; i < RAFTABLE_PEERS_MAX; i++)
	{
		HostPort *hp = cfg->peers + i;
		if (!hp->up) continue;

		if (i == cfg->id)
		{
			raft_peer_up(raft, i, hp->host, hp->port, true);
			server.host = hp->host;
			server.port = hp->port;
		}
		else
			raft_peer_up(raft, i, hp->host, hp->port, false);
	}
}

/* Returns the created socket, or -1 if failed. */
static int create_listening_socket(const char *host, int port) {
	int optval;
	struct sockaddr_in addr;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == -1) {
		fprintf(stderr, "cannot create the listening socket: %s\n", strerror(errno));
		return -1;
	}

	optval = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char const*)&optval, sizeof(optval));
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char const*)&optval, sizeof(optval));

	addr.sin_family = AF_INET;
	if (inet_aton(host, &addr.sin_addr) == 0) {
		fprintf(stderr, "cannot convert the host string '%s' to a valid address\n", host);
		return -1;
	}
	addr.sin_port = htons(port);
	fprintf(stderr, "binding tcp %s:%d\n", host, port);
	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		fprintf(stderr, "cannot bind the listening socket: %s\n", strerror(errno));
		return -1;
	}

	if (listen(s, LISTEN_QUEUE_SIZE) == -1) {
		fprintf(stderr, "failed to listen the socket: %s\n", strerror(errno));
		return -1;
	}

	return s;
}

static bool add_socket(int sock)
{
	FD_SET(sock, &server.all);
	if (sock > server.maxfd) {
		server.maxfd = sock;
	}
	return true;
}

static bool add_client(int sock)
{
	int i;

	if (server.clientnum >= MAX_CLIENTS)
	{
		fprintf(stderr, "client limit hit\n");
		return false;
	}

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		Client *c = server.clients + i;
		if (c->sock >= 0) continue;

		c->sock = sock;
		c->good = true;
		c->msg = NULL;
		c->bufrecved = 0;
		c->expect.id = NOBODY;
		server.clientnum++;
		return add_socket(sock);
	}

	Assert(false); // should not happen
	return false;
}

static bool remove_socket(int sock)
{
	FD_CLR(sock, &server.all);
	return true;
}

static bool remove_client(Client *c)
{
	int sock = c->sock;
	Assert(sock >= 0);
	c->sock = -1;
	if (c->msg) pfree(c->msg);

	server.clientnum--;
	close(sock);
	return remove_socket(sock);
}

static bool start_server(void)
{
	int i;

	server.listener = -1;
	server.raftsock = -1;
	FD_ZERO(&server.all);
	server.maxfd = 0;
	server.clientnum = 0;

	server.listener = create_listening_socket(server.host, server.port);
	if (server.listener == -1)
	{
		return false;
	}

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		server.clients[i].sock = -1;
	}

	return add_socket(server.listener);
}

static bool accept_client(void)
{
	int fd;

	fprintf(stderr, "a new connection is queued\n");

	fd = accept(server.listener, NULL, NULL);
	if (fd == -1) {
		fprintf(stderr, "failed to accept a connection: %s\n", strerror(errno));
		return false;
	}
	fprintf(stderr, "a new connection fd=%d accepted\n", fd);
	
	if (!raft_is_leader(raft)) {
		fprintf(stderr, "not a leader, disconnecting the accepted connection fd=%d\n", fd);
		close(fd);
		return false;
	}

	return add_client(fd);
}

static bool pull_from_socket(Client *c)
{
	if (!c->good) return false;
	Assert(c->sock >= 0);
	void *dst = c->buf + c->bufrecved;
	size_t avail = BUFLEN - c->bufrecved;
	if (!avail) return false;

	size_t recved = recv(c->sock, dst, avail, MSG_DONTWAIT);
	if (recv <= 0)
	{
		c->good = false;
		return false;
	}
	c->bufrecved += recved;

	return true;
}

static void shift_buffer(Client *c, size_t bytes)
{
	Assert(c->bufrecved >= bytes);
	memmove(c->buf, c->buf + bytes, c->bufrecved - bytes);
	c->bufrecved -= bytes;
}

static int extract_nomore(Client *c, void *dst, size_t bytes)
{
	if (c->bufrecved < bytes) bytes = c->bufrecved;
	
	memcpy(dst, c->buf, bytes);
	shift_buffer(c, bytes);

	return bytes;
}

static bool extract_exactly(Client *c, void *dst, size_t bytes)
{
	if (c->bufrecved < bytes) return false;

	memcpy(dst, c->buf, bytes);
	shift_buffer(c, bytes);
	return true;
}

static bool get_new_message(Client *c)
{
	Assert((!c->msg) || (c->msgrecved < c->msglen));
	if (!c->msg) // need to allocate the memory for the message
	{
		if (!extract_exactly(c, &c->msglen, sizeof(c->msglen)))
			return false; // but the size is still unknown

		c->msg = palloc(c->msglen);
		c->msgrecved = 0;
	}

	if (c->msgrecved < c->msglen)
		c->msgrecved += extract_nomore(c, c->msg + c->msgrecved, c->msglen - c->msgrecved);
	Assert(c->msgrecved <= c->msglen);
	return c->msgrecved == c->msglen;
}

static void attend(Client *c)
{
	if (!c->good) return;
	if (!pull_from_socket(c)) return;
	while (get_new_message(c))
	{
		raft_update_t u;
		RaftableUpdate *ru = (RaftableUpdate *)c->msg;

		Assert(c->expect.id == NOBODY); /* client shouldn't send multiple updates at once */

		u.len = c->msglen;
		u.data = c->msg;
		c->expect.id = ru->expector;
		int index = raft_emit(raft, u);
		if (index >= 0)
			c->expect.index = index;
		else
			c->good = false;
		pfree(c->msg);
		c->msg = NULL;
	}
}

static void notify(void)
{
	int i = 0;
	for (i = 0; i < MAX_CLIENTS; i++)
	{
		Client *c = server.clients + i;
		if (c->sock < 0) continue;
		if (!c->good) continue;
		if (c->expect.id == NOBODY) continue;
		if (!raft_applied(raft, c->expect.id, c->expect.index)) continue;

		int ok = 1;
		if (send(c->sock, &ok, sizeof(ok), 0) != sizeof(ok))
		{
			fprintf(stderr, "failed to notify client\n");
			c->good = false;
		}
		c->expect.id = NOBODY;
	}
}

static void drop_bads(void)
{
	int i;
	for (i = 0; i < MAX_CLIENTS; i++)
	{
		Client *c = server.clients + i;
		if (c->sock < 0) continue;
		if (!c->good || !raft_is_leader(raft)) remove_client(c);
	}
}

static bool tick(int timeout_ms)
{
	int numready;
	bool raft_ready = false;

	fd_set readfds = server.all;
	struct timeval timeout = ms2tv(timeout_ms);
	numready = select(server.maxfd + 1, &readfds, NULL, NULL, &timeout);
	if (numready == -1)
	{
		fprintf(stderr, "failed to select: %s\n", strerror(errno));
		return false;
	}

	if (FD_ISSET(server.listener, &readfds))
	{
		numready--;
		accept_client();
	}

	if (FD_ISSET(server.raftsock, &readfds))
	{
		numready--;
		raft_ready = true;
	}

	Client *c = server.clients;
	while (numready > 0)
	{
		Assert(c - server.clients < MAX_CLIENTS);
		if ((c->sock >= 0) && (FD_ISSET(c->sock, &readfds)))
		{
			attend(c);
			numready--;
		}
		c++;
	}

	drop_bads();

	return raft_ready;
}


static int stop = 0;
static void die(int sig)
{
    stop = 1;
}

static void worker_main(Datum arg)
{
	WorkerConfig *cfg = (WorkerConfig *)(arg);
	StateP state = (StateP)cfg->getter();

	cfg->raft_config.userdata = state;
	cfg->raft_config.applier = applier;
	cfg->raft_config.snapshooter = snapshooter;

	raft = raft_init(&cfg->raft_config);
	if (raft == NULL)
		elog(ERROR, "couldn't configure raft");

	add_peers(cfg);

	if (!start_server()) elog(ERROR, "couldn't start raftable server");

    signal(SIGINT, die);
    signal(SIGQUIT, die);
    signal(SIGTERM, die);
    sigset_t sset;
    sigfillset(&sset);
    sigprocmask(SIG_UNBLOCK, &sset, NULL);

	server.raftsock = raft_create_udp_socket(raft);
	add_socket(server.raftsock);
	add_socket(server.listener);
	if (server.raftsock == -1) elog(ERROR, "couldn't start raft");

	mstimer_t t;
	mstimer_reset(&t);
	while (!stop)
	{
		raft_msg_t m = NULL;

		int ms = mstimer_reset(&t);
		raft_tick(raft, ms);

		if (tick(cfg->raft_config.heartbeat_ms))
		{
			m = raft_recv_message(raft);
			Assert(m != NULL);
			raft_handle_message(raft, m);
			notify();
		}
	}
}

void worker_register(WorkerConfig *cfg)
{
	BackgroundWorker worker = {};
	strcpy(worker.bgw_name, "raftable worker");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = 1;
	worker.bgw_main = worker_main;
	worker.bgw_main_arg = PointerGetDatum(cfg);
	RegisterBackgroundWorker(&worker);
}


void parse_peers(HostPort *peers, char *peerstr)
{
	char *state, *substate;
	char *peer, *s;
	char *host;
	int id, port;
	int i;

	for (i = 0; i < RAFTABLE_PEERS_MAX; i++)
		peers[i].up = false;

	fprintf(stderr, "parsing '%s'\n", peerstr);
	peer = strtok_r(peerstr, ",", &state);
	while (peer)
	{
		fprintf(stderr, "peer = '%s'\n", peer);

		s = strtok_r(peer, ":", &substate);
		if (!s) break;
		id = atoi(s);
		fprintf(stderr, "id = %d ('%s')\n", id, s);

		host = strtok_r(NULL, ":", &substate);
		if (!host) break;
		fprintf(stderr, "host = '%s'\n", host);

		s = strtok_r(NULL, ":", &substate);
		if (!s) break;
		port = atoi(s);
		fprintf(stderr, "port = %d ('%s')\n", port, s);

		Assert(!peers[id].up);
		peers[id].up = true;
		peers[id].port = port;
		strncpy(peers[id].host, host, sizeof(peers[id].host));

		peer = strtok_r(NULL, ",", &state);
	}
}