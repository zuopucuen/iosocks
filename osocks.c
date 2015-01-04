/*
 * osocks.c - iosocks server
 *
 * Copyright (C) 2014, Xiaoxiao <i@xiaoxiao.im>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <ev.h>
#include "conf.h"
#include "encrypt.h"
#include "log.h"
#include "md5.h"
#include "mem.h"

// 缓冲区大小
#define BUF_SIZE 8192

// 魔数
#define MAGIC 0x526f6e61

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

// 连接状态
typedef enum
{
	CLOSED = 0,
	REQ_RCVD,
	REQ_ERR,
	CONNECTED,
	ESTAB,
	CLOSE_WAIT
} state_t;


// 域名解析控制块
typedef struct
{
	struct gaicb req;
	struct addrinfo hints;
	struct addrinfo *res;
	char host[264];
	char port[8];
} gai_t;

// 连接控制块
typedef struct
{
	ev_io w_local_read;
	ev_io w_local_write;
	ev_io w_remote_read;
	ev_io w_remote_write;
	ssize_t rx_bytes;
	ssize_t tx_bytes;
	ssize_t rx_offset;
	ssize_t tx_offset;
	gai_t *gai;
	int server_index;
	int sock_local;
	int sock_remote;
	state_t state;
	enc_evp_t enc_evp;
	uint8_t rx_buf[BUF_SIZE];
	uint8_t tx_buf[BUF_SIZE];
} conn_t;


static void help(void);
static void signal_cb(EV_P_ ev_signal *w, int revents);
static void accept_cb(EV_P_ ev_io *w, int revents);
static void local_read_cb(EV_P_ ev_io *w, int revents);
static void local_write_cb(EV_P_ ev_io *w, int revents);
static void remote_read_cb(EV_P_ ev_io *w, int revents);
static void remote_write_cb(EV_P_ ev_io *w, int revents);
static void connect_cb(EV_P_ ev_io *w, int revents);
static void resolv_cb(int signo, siginfo_t *info, void *context);
static void closewait_cb(EV_P_ ev_timer *w, int revents);
static void cleanup(EV_P_ conn_t *conn);
static int setnonblock(int sock);
static int settimeout(int sock);
static int setreuseaddr(int sock);

// 服务器的信息
typedef struct
{
	char *key;
	size_t key_len;
} server_t;
server_t servers[MAX_SERVER];

struct ev_loop *loop;

int main(int argc, char **argv)
{
	const char *conf_file = NULL;
	conf_t conf;
	bzero(&conf, sizeof(conf_t));

	// 处理命令行参数
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
		{
			help();
			return 0;
		}
		else if (strcmp(argv[i], "-c") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf_file = argv[i + 1];
			i++;
		}
		else if (strcmp(argv[i], "-s") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf.server_num = 1;
			conf.server[0].address = argv[i + 1];
			i++;
		}
		else if (strcmp(argv[i], "-p") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf.server_num = 1;
			conf.server[0].port = argv[i + 1];
			i++;
		}
		else if (strcmp(argv[i], "-b") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf.local.address = argv[i + 1];
			i++;
		}
		else if (strcmp(argv[i], "-l") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf.local.port = argv[i + 1];
			i++;
		}
		else if (strcmp(argv[i], "-k") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf.server_num = 1;
			conf.server[0].key = argv[i + 1];
			i++;
		}
		else
		{
			fprintf(stderr, "Invalid option: %s\n", argv[i]);
			return 1;
		}
	}
	if (conf_file != NULL)
	{
		if (read_conf(conf_file, &conf) != 0)
		{
			return 1;
		}
	}
	if (conf.server_num == 0)
	{
		help();
		return 1;
	}
	for (int i = 0; i < conf.server_num; i++)
	{
		if (conf.server[i].address == NULL)
		{
			conf.server[i].address = "0.0.0.0";
		}
		if (conf.server[i].port == NULL)
		{
			conf.server[i].port = "1205";
		}
		if (conf.server[i].key == NULL)
		{
			help();
			return 1;
		}
	}
	if (conf.local.address == NULL)
	{
		conf.local.address = "127.0.0.1";
	}
	if (conf.local.port == NULL)
	{
		conf.local.port = "1080";
	}

	// 服务器信息
	for (int i = 0; i < conf.server_num; i++)
	{
		servers[i].key = conf.server[i].key;
		servers[i].key_len = strlen(servers[i].key);
		if (servers[i].key_len > 256)
		{
			servers[i].key[257] = '\0';
			servers[i].key_len = 256;
		}
	}

	// 初始化内存池
	size_t block_size[3] = { sizeof(ev_timer), sizeof(gai_t), sizeof(conn_t)};
	size_t block_count[3] = { 8, 8, 64};
	if (mem_init(block_size, block_count, 3) != 0)
	{
		LOG("memory pool error");
		return 3;
	}

	// 初始化 ev_signal
	loop = EV_DEFAULT;
	ev_signal w_sigint;
	ev_signal w_sigterm;
	ev_signal_init(&w_sigint, signal_cb, SIGINT);
	ev_signal_init(&w_sigterm, signal_cb, SIGTERM);
	ev_signal_start(EV_A_ &w_sigint);
	ev_signal_start(EV_A_ &w_sigterm);

	// SIGUSR1 信号
	struct sigaction sa;
	sa.sa_handler = (void(*) (int))resolv_cb;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	if (sigaction(SIGUSR1, &sa, NULL) != 0)
	{
		LOG("failed to setup SIGUSR1 handler");
		return 4;
	}

	// 初始化本地监听 socket
	int sock_listen[MAX_SERVER];
	ev_io w_listen[MAX_SERVER];
	struct addrinfo hints;
	struct addrinfo *res;
	for (int i = 0; i < conf.server_num; i++)
	{
		bzero(&hints, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(conf.server[i].address, conf.server[i].port, &hints, &res) != 0)
		{
			LOG("wrong server_host/server_port");
			return 2;
		}
		sock_listen[i] = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if (sock_listen[i] < 0)
		{
			ERR("socket");
			return 2;
		}
		setnonblock(sock_listen[i]);
		setreuseaddr(sock_listen[i]);
		if (bind(sock_listen[i], (struct sockaddr *)res->ai_addr, res->ai_addrlen) != 0)
		{
			ERR("bind");
			return 2;
		}
		freeaddrinfo(res);
		if (listen(sock_listen[i], 1024) != 0)
		{
			ERR("listen");
			return 2;
		}
		ev_io_init(&(w_listen[i]), accept_cb, sock_listen[i], EV_READ);
		w_listen[i].data = (void *)(long)i;
		ev_io_start(EV_A_ &(w_listen[i]));
		LOG("starting osocks at %s:%s", conf.server[i].address, conf.server[i].port);
	}

	// 执行事件循环
	ev_run(loop, 0);

	// 退出
	LOG("Exit");
	for (int i = 0; i < conf.server_num; i++)
	{
		close(sock_listen[i]);
	}

	return 0;
}

static void help(void)
{
	printf("usage: osocks\n"
		   "  -h, --help        show this help\n"
		   "  -s <server_addr>  server address, default: 0.0.0.0\n"
		   "  -p <server_port>  server port, default: 1205\n"
		   "  -k <key>          encryption key\n"
		   "");
}

static void signal_cb(EV_P_ ev_signal *w, int revents)
{
	assert((w->signum == SIGINT) || (w->signum == SIGTERM));
	ev_break(EV_A_ EVBREAK_ALL);
}

static void accept_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)mem_new(sizeof(conn_t));
	if (conn == NULL)
	{
		LOG("out of memory");
		return;
	}
	conn->sock_local = accept(w->fd, NULL, NULL);
	if (conn->sock_local < 0)
	{
		ERR("accept");
		mem_delete(conn);
		return;
	}
	setnonblock(conn->sock_local);
	settimeout(conn->sock_local);
	conn->server_index = (int)(long)(w->data);
	conn->state = CLOSED;
	ev_io_init(&conn->w_local_read, local_read_cb, conn->sock_local, EV_READ);
	ev_io_init(&conn->w_local_write, local_write_cb, conn->sock_local, EV_WRITE);
	conn->w_local_read.data = (void *)conn;
	conn->w_local_write.data = (void *)conn;
	ev_io_start(EV_A_ &conn->w_local_read);
}

static void local_read_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);

	if (conn->state != ESTAB)
	{
		ev_io_stop(EV_A_ w);
	}

	switch (conn->state)
	{
	case CLOSED:
	{
		// iosocks 请求
		// +-------+------+------+------+
		// | MAGIC | HOST | PORT |  IV  |
		// +-------+------+------+------+
		// |   4   | 257  |  15  | 236  |
		// +-------+------+------+------+
		uint8_t key[64];
		ssize_t rx_bytes = recv(conn->sock_local, conn->rx_buf, BUF_SIZE, 0);
		if (rx_bytes != 512)
		{
			if (rx_bytes < 0)
			{
#ifndef NDEBUG
				ERR("recv");
#endif
				LOG("client reset");
			}
			close(conn->sock_local);
			mem_delete(conn);
			return;
		}
		memcpy(conn->tx_buf, conn->rx_buf + 276, 236);
		memcpy(conn->tx_buf + 236, servers[conn->server_index].key, servers[conn->server_index].key_len);
		md5(conn->tx_buf, 236 + servers[conn->server_index].key_len, key);
		md5(key, 16, key + 16);
		md5(key, 32, key + 32);
		md5(key, 48, key + 48);
		enc_init(&conn->enc_evp, enc_rc4, key, 64);
		io_decrypt(conn->rx_buf, 276, &conn->enc_evp);
		uint32_t magic = ntohl(*((uint32_t *)(conn->rx_buf)));
		const char *host = (const char *)conn->rx_buf + 4;
		const char *port = (const char *)conn->rx_buf + 261;
		conn->rx_buf[260] = 0;
		conn->rx_buf[275] = 0;
		LOG("connect %s:%s", host, port);
		if (magic != MAGIC)
		{
			LOG("illegal client");
			close(conn->sock_local);
			mem_delete(conn);
			return;
		}
		conn->gai = (gai_t *)mem_new(sizeof(gai_t));
		if (conn->gai == NULL)
		{
			LOG("out of memory");
			close(conn->sock_local);
			mem_delete(conn);
			return;
		}
		conn->gai->hints.ai_family = AF_UNSPEC;
		conn->gai->hints.ai_socktype = SOCK_STREAM;
		strcpy(conn->gai->host, host);
		strcpy(conn->gai->port, port);
		bzero(&(conn->gai->req), sizeof(struct gaicb));
		conn->gai->req.ar_name = conn->gai->host;
		conn->gai->req.ar_service = conn->gai->port;
		conn->gai->req.ar_request = &(conn->gai->hints);
		conn->gai->req.ar_result = NULL;
		struct gaicb *req_ptr = &(conn->gai->req);
		struct sigevent sevp;
		sevp.sigev_notify = SIGEV_SIGNAL;
		sevp.sigev_signo = SIGUSR1;
		sevp.sigev_value.sival_ptr = (void *)conn;
		if (getaddrinfo_a(GAI_NOWAIT, &req_ptr, 1, &sevp) != 0)
		{
			close(conn->sock_local);
			mem_delete(conn);
			return;
		}
		break;
	}
	case ESTAB:
	{
		conn->tx_bytes = recv(conn->sock_local, conn->tx_buf, BUF_SIZE, 0);
		if (conn->tx_bytes <= 0)
		{
			if (conn->tx_bytes < 0)
			{
#ifndef NDEBUG
				ERR("recv");
#endif
				LOG("client reset");
			}
			cleanup(EV_A_ conn);
			return;
		}
		io_decrypt(conn->tx_buf, conn->tx_bytes, &conn->enc_evp);
		ssize_t n = send(conn->sock_remote, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
		if (n < 0)
		{
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			{
				conn->tx_offset = 0;
			}
			else
			{
				ERR("send");
				cleanup(EV_A_ conn);
				return;
			}
		}
		else if (n < conn->tx_bytes)
		{
			conn->tx_offset = n;
			conn->tx_bytes -= n;
		}
		else
		{
			return;
		}
		ev_io_start(EV_A_ &conn->w_remote_write);
		ev_io_stop(EV_A_ w);
		break;
	}
	default:
	{
		// 不应该来到这里
		assert(0 != 0);
		break;
	}
	}
}

static void local_write_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)w->data;

	assert(conn != NULL);

	if (conn->state != ESTAB)
	{
		ev_io_stop(EV_A_ w);
	}

	switch (conn->state)
	{
	case REQ_ERR:
	case CONNECTED:
	{
		ssize_t tx_bytes = send(conn->sock_local, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
		if (tx_bytes != conn->tx_bytes)
		{
			if (tx_bytes < 0)
			{
				ERR("send");
			}
			close(conn->sock_local);
			mem_delete(conn->gai);
			mem_delete(conn);
			return;
		}
		if (conn->state == CONNECTED)
		{
			conn->state = ESTAB;
			ev_io_init(&conn->w_remote_read, remote_read_cb, conn->sock_remote, EV_READ);
			ev_io_init(&conn->w_remote_write, remote_write_cb, conn->sock_remote, EV_WRITE);
			conn->w_remote_read.data = (void *)conn;
			conn->w_remote_write.data = (void *)conn;
			ev_io_start(EV_A_ &conn->w_local_read);
			ev_io_start(EV_A_ &conn->w_remote_read);
		}
		else
		{
			conn->state = CLOSE_WAIT;
			ev_timer *w_timer = (ev_timer *)mem_new(sizeof(ev_timer));
			if (w_timer == NULL)
			{
				close(conn->sock_local);
				mem_delete(conn->gai);
				mem_delete(conn);
				return;
			}
			ev_timer_init(w_timer, closewait_cb, 1.0, 0);
			w_timer->data = (void *)conn;
			ev_timer_start(EV_A_ w_timer);
		}
		break;
	}
	case ESTAB:
	{
		assert(conn->rx_bytes > 0);
		ssize_t n = send(conn->sock_local, conn->rx_buf + conn->rx_offset, conn->rx_bytes, MSG_NOSIGNAL);
		if (n < 0)
		{
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			{
				return;
			}
			else
			{
				ERR("send");
				cleanup(EV_A_ conn);
				return;
			}
		}
		else if (n < conn->rx_bytes)
		{
			conn->rx_offset += n;
			conn->rx_bytes -= n;
		}
		else
		{
			ev_io_start(EV_A_ &conn->w_remote_read);
			ev_io_stop(EV_A_ w);
		}
		break;
	}
	default:
	{
		// 不应该来到这里
		assert(0 != 0);
		break;
	}
	}
}

static void remote_read_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);
	assert(conn->state == ESTAB);

	conn->rx_bytes = recv(conn->sock_remote, conn->rx_buf, BUF_SIZE, 0);
	if (conn->rx_bytes <= 0)
	{
		if (conn->rx_bytes < 0)
		{
#ifndef NDEBUG
			ERR("recv");
#endif
			LOG("remote server reset");
		}
		cleanup(EV_A_ conn);
		return;
	}
	io_encrypt(conn->rx_buf, conn->rx_bytes, &conn->enc_evp);
	ssize_t n = send(conn->sock_local, conn->rx_buf, conn->rx_bytes, MSG_NOSIGNAL);
	if (n < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			conn->rx_offset = 0;
		}
		else
		{
			ERR("send");
			cleanup(EV_A_ conn);
			return;
		}
	}
	else if (n < conn->rx_bytes)
	{
		conn->rx_offset = n;
		conn->rx_bytes -= n;
	}
	else
	{
		return;
	}
	ev_io_start(EV_A_ &conn->w_local_write);
	ev_io_stop(EV_A_ w);
}

static void remote_write_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);
	assert(conn->state == ESTAB);
	assert(conn->tx_bytes > 0);

	ssize_t n = send(conn->sock_remote, conn->tx_buf + conn->tx_offset, conn->tx_bytes, MSG_NOSIGNAL);
	if (n < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			return;
		}
		else
		{
			ERR("send");
			cleanup(EV_A_ conn);
			return;
		}
	}
	else if (n < conn->tx_bytes)
	{
		conn->tx_offset += n;
		conn->tx_bytes -= n;
	}
	else
	{
		ev_io_start(EV_A_ &conn->w_local_read);
		ev_io_stop(EV_A_ w);
	}
}


static void resolv_cb(int signo, siginfo_t *info, void *context)
{
	conn_t *conn = (conn_t *)info->si_value.sival_ptr;

	assert(signo == SIGUSR1);
	assert(conn != NULL);

	if (gai_error(&conn->gai->req) != 0)
	{
		// 域名解析失败
		LOG("can not resolv host: %s", conn->gai->host);
		mem_delete(conn->gai);
		// iosocks 应答
		// +-------+
		// | MAGIC |
		// +-------+
		// |   4   |
		// +-------+
		bzero(conn->tx_buf, 4);
		conn->tx_bytes = 4;
		io_encrypt(conn->tx_buf, conn->tx_bytes, &conn->enc_evp);
		conn->state = REQ_ERR;
		ev_io_start(EV_A_ &conn->w_local_write);
	}
	else
	{
		// 域名解析成功，建立远程连接
		conn->gai->res = conn->gai->req.ar_result;
		conn->sock_remote = socket(conn->gai->res->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if (conn->sock_remote < 0)
		{
			ERR("socket");
			close(conn->sock_local);
			mem_delete(conn->gai);
			mem_delete(conn);
			return;
		}
		setnonblock(conn->sock_remote);
		settimeout(conn->sock_remote);
		ev_io_init(&conn->w_remote_write, connect_cb, conn->sock_remote, EV_WRITE);
		conn->w_remote_write.data = (void *)conn;
		ev_io_start(EV_A_ &conn->w_remote_write);
		conn->state = REQ_RCVD;
		connect(conn->sock_remote, (struct sockaddr *)conn->gai->res->ai_addr, conn->gai->res->ai_addrlen);
	}
}

static void connect_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);
	assert(conn->state == REQ_RCVD);

	ev_io_stop(EV_A_ w);

	int error = 1;
	socklen_t len = sizeof(int);
	getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &error, &len);

	// iosocks 应答
	// +-------+
	// | MAGIC |
	// +-------+
	// |   4   |
	// +-------+
	if (error == 0)
	{
		// 连接成功
		*((uint32_t *)(conn->tx_buf)) = htonl(MAGIC);
		conn->state = CONNECTED;
	}
	else
	{
		// 连接失败
		close(conn->sock_remote);
		conn->gai->res = conn->gai->res->ai_next;
		if (conn->gai->res != NULL)
		{
			conn->sock_remote = socket(conn->gai->res->ai_family, SOCK_STREAM, IPPROTO_TCP);
			if (conn->sock_remote < 0)
			{
				ERR("socket");
				close(conn->sock_local);
				freeaddrinfo(conn->gai->req.ar_result);
				mem_delete(conn->gai);
				mem_delete(conn);
				return;
			}
			setnonblock(conn->sock_remote);
			settimeout(conn->sock_remote);
			ev_io_init(&conn->w_remote_write, connect_cb, conn->sock_remote, EV_WRITE);
			conn->w_remote_write.data = (void *)conn;
			ev_io_start(EV_A_ &conn->w_remote_write);
			connect(conn->sock_remote, (struct sockaddr *)conn->gai->res->ai_addr, conn->gai->res->ai_addrlen);
			return;
		}
		else
		{
			LOG("connect failed");
			close(conn->sock_remote);
			*((uint32_t *)(conn->tx_buf)) = 0;
			conn->state = REQ_ERR;
		}
	}
	freeaddrinfo(conn->gai->req.ar_result);
	mem_delete(conn->gai);
	conn->tx_bytes = 4;
	io_encrypt(conn->tx_buf, conn->tx_bytes, &conn->enc_evp);
	ev_io_stop(EV_A_ w);
	ev_io_start(EV_A_ &conn->w_local_write);
}

static void closewait_cb(EV_P_ ev_timer *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);
	assert(conn->state == CLOSE_WAIT);

	ev_timer_stop(EV_A_ w);
	close(conn->sock_local);
	mem_delete(w);
	mem_delete(conn);
}

static void cleanup(EV_P_ conn_t *conn)
{
	ev_io_stop(EV_A_ &conn->w_local_read);
	ev_io_stop(EV_A_ &conn->w_local_write);
	ev_io_stop(EV_A_ &conn->w_remote_read);
	ev_io_stop(EV_A_ &conn->w_remote_write);
	close(conn->sock_local);
	close(conn->sock_remote);
	mem_delete(conn);
}

static int setnonblock(int sock)
{
	int flags;
	flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1)
	{
		return -1;
	}
	if (-1 == fcntl(sock, F_SETFL, flags | O_NONBLOCK))
	{
		return -1;
	}
	return 0;
}

static int settimeout(int sock)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0};
	if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(struct timeval)) != 0)
	{
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(struct timeval)) != 0)
	{
		return -1;
	}
	return 0;
}

static int setreuseaddr(int sock)
{
	int reuseaddr = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int)) != 0)
	{
		return -1;
	}
	return 0;
}
