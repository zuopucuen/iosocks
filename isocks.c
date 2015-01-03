/*
 * isocks.c - iosocks client
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

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
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
	NEGO_RCVD,
	NEGO_ERR,
	NEGO_SENT,
	CMD_RCVD,
	CMD_ERR,
	CONNECTED,
	REQ_SENT,
	REP_RCVD,
	REQ_ERR,
	ESTAB,
	CLOSE_WAIT
} state_t;

// 连接控制块结构
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
static void connect_cb(EV_P_ ev_io *w, int revents);
static void local_read_cb(EV_P_ ev_io *w, int revents);
static void local_write_cb(EV_P_ ev_io *w, int revents);
static void remote_read_cb(EV_P_ ev_io *w, int revents);
static void remote_write_cb(EV_P_ ev_io *w, int revents);
static void closewait_cb(EV_P_ ev_timer *w, int revents);
static int setnonblock(int sock);
static int settimeout(int sock);
static void rand_bytes(void *stream, size_t len);
static void cleanup(EV_P_ conn_t *conn);

// 配置信息
conf_t conf;

// 服务器的信息
struct
{
	char addr[128];
	socklen_t addrlen;
	int family;
	char *key;
	size_t key_len;
} servers[MAX_SERVER];

int main(int argc, char **argv)
{
	const char *conf_file = NULL;

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
	struct addrinfo hints;
	struct addrinfo *res;
	for (int i = 0; i < conf.server_num; i++)
	{
		servers[i].key = conf.server[i].key;
		servers[i].key_len = strlen(servers[i].key);
		if (servers[i].key_len > 256)
		{
			servers[i].key[257] = '\0';
			servers[i].key_len = 256;
		}
		bzero(&hints, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(conf.server[i].address, conf.server[i].port, &hints, &res) != 0)
		{
			LOG("wrong server_host/server_port");
			return 2;
		}
		memcpy(servers[i].addr, res->ai_addr, res->ai_addrlen);
		servers[i].addrlen = res->ai_addrlen;
		servers[i].family = res->ai_family;
		freeaddrinfo(res);
	}

	// 初始化本地监听 socket
	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(conf.local.address, conf.local.port, &hints, &res) != 0)
	{
		LOG("wrong local_host/local_port");
		return 2;
	}
	int sock_listen = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
	if (sock_listen < 0)
	{
		ERR("socket");
		return 2;
	}
	setnonblock(sock_listen);
	int reuseaddr = 1;
	setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int));
	if (bind(sock_listen, (struct sockaddr *)res->ai_addr, res->ai_addrlen) != 0)
	{
		ERR("bind");
		return 2;
	}
	freeaddrinfo(res);
	if (listen(sock_listen, 1024) != 0)
	{
		ERR("listen");
		return 2;
	}

	// 初始化内存池
	size_t block_size[2] = { sizeof(ev_timer), sizeof(conn_t) };
	size_t block_count[2] = { 8, 64 };
	if (mem_init(block_size, block_count, 2) != 0)
	{
		LOG("memory pool error");
		return 3;
	}

	// 初始化 ev
	struct ev_loop *loop = EV_DEFAULT;
	ev_signal w_sigint;
	ev_signal w_sigterm;
	ev_signal_init(&w_sigint, signal_cb, SIGINT);
	ev_signal_init(&w_sigterm, signal_cb, SIGTERM);
	ev_signal_start(loop, &w_sigint);
	ev_signal_start(loop, &w_sigterm);
	ev_io w_listen;
	ev_io_init(&w_listen, accept_cb, sock_listen, EV_READ);
	ev_io_start(loop, &w_listen);

	// 执行事件循环
	LOG("starting isocks at %s:%s", conf.local.address, conf.local.port);
	ev_run(loop, 0);

	// 退出
	close(sock_listen);
	LOG("Exit");

	return 0;
}

static void help(void)
{
	printf("usage: isocks\n"
		   "  -h, --help        show this help\n"
		   "  -s <server_addr>  server address, default: 0.0.0.0\n"
		   "  -p <server_port>  server port, default: 1205\n"
		   "  -b <local_addr>   local binding address, default: 127.0.0.1\n"
		   "  -l <local_port>   local port, default: 1080\n"
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
		// 协商请求
		// +-----+----------+----------+
		// | VER | NMETHODS | METHODS  |
		// +-----+----------+----------+
		// |  1  |    1     | 1 to 255 |
		// +-----+----------+----------+
		bzero(conn->rx_buf, 257);
		ssize_t rx_bytes = recv(conn->sock_local, conn->rx_buf, BUF_SIZE, 0);
		if (rx_bytes <= 0)
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
		int error = 0;
		if (conn->rx_buf[0] != 0x05)
		{
			error = 1;
		}
		uint8_t nmethods = conn->rx_buf[1];
		uint8_t i;
		for (i = 0; i < nmethods; i++)
		{
			if (conn->rx_buf[2 + i] == 0x00)
			{
				break;
			}
		}
		if (i >= nmethods)
		{
			error = 2;
		}
		// 协商回应
		// +-----+--------+
		// | VER | METHOD |
		// +-----+--------+
		// |  1  |   1    |
		// +-----+--------+
		conn->tx_buf[0] = 0x05;
		conn->tx_buf[1] = 0x00;
		conn->tx_bytes = 2;
		conn->state = NEGO_RCVD;
		if (error != 0)
		{
			conn->state = NEGO_ERR;
			conn->tx_buf[1] = 0xff;
		}
		ev_io_start(EV_A_ &conn->w_local_write);
		break;
	}
	case NEGO_SENT:
	{
		// 命令请求
		// +-----+-----+-------+------+----------+----------+
		// | VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
		// +-----+-----+-------+------+----------+----------+
		// |  1  |  1  | X'00' |  1   | Variable |    2     |
		// +-----+-----+-------+------+----------+----------+
		bzero(conn->rx_buf, 263);
		ssize_t rx_bytes = recv(conn->sock_local, conn->rx_buf, BUF_SIZE, 0);
		if (rx_bytes <= 0)
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
		int error = 0;
		if (conn->rx_buf[0] != 0x05)
		{
			error = 1;
		}
		if (conn->rx_buf[1] != 0x01)
		{
			// 只支持 CONNECT 命令
			error = 2;
		}
		char host[257], port[15];
		if (conn->rx_buf[3] == 0x01)
		{
			// IPv4 地址
			inet_ntop(AF_INET, (const void *)(conn->rx_buf + 4), host, INET_ADDRSTRLEN);
			sprintf(port, "%u", ntohs(*(uint16_t *)(conn->rx_buf + 8)));
		}
		else if (conn->rx_buf[3] == 0x03)
		{
			// 域名
			memcpy(host, conn->rx_buf + 5, conn->rx_buf[4]);
			host[conn->rx_buf[4]] = '\0';
			sprintf(port, "%u", ntohs(*(uint16_t *)(conn->rx_buf + conn->rx_buf[4] + 5)));
		}
		else if (conn->rx_buf[3] == 0x04)
		{
			// IPv6 地址
			inet_ntop(AF_INET6, (const void *)(conn->rx_buf + 4), host, INET6_ADDRSTRLEN);
			sprintf(port, "%u", ntohs(*(uint16_t *)(conn->rx_buf + 20)));
		}
		else
		{
			// 不支持的地址类型
			error = 3;
		}
		if (error == 0)
		{
			LOG("connect %s:%s", host, port);
			// 随机选择一个 server
			unsigned int index;
			rand_bytes(&index, sizeof(unsigned int));
			index %= conf.server_num;
			// iosocks 请求
			// +-------+------+------+------+
			// | MAGIC | HOST | PORT |  IV  |
			// +-------+------+------+------+
			// |   4   | 257  |  15  | 236  |
			// +-------+------+------+------+
			uint8_t key[64];
			rand_bytes(conn->rx_buf, 236);
			memcpy(conn->rx_buf + 236, servers[index].key, servers[index].key_len);
			md5(conn->rx_buf, 236 + servers[index].key_len, key);
			md5(key, 16, key + 16);
			md5(key, 32, key + 32);
			md5(key, 48, key + 48);
			enc_init(&conn->enc_evp, enc_rc4, key, 64);
			memcpy(conn->tx_buf + 276, conn->rx_buf, 236);
			bzero(conn->tx_buf, 276);
			*((uint32_t *)(conn->tx_buf)) = htonl(MAGIC);
			strcpy((char *)conn->tx_buf + 4, host);
			strcpy((char *)conn->tx_buf + 261, port);
			io_encrypt(conn->tx_buf, 276, &conn->enc_evp);
			conn->tx_bytes = 512;
			// 建立远程连接
			conn->sock_remote = socket(servers[index].family, SOCK_STREAM, IPPROTO_TCP);
			if (conn->sock_remote < 0)
			{
				ERR("socket");
				close(conn->sock_local);
				mem_delete(conn);
				return;
			}
			setnonblock(conn->sock_remote);
			settimeout(conn->sock_remote);
			ev_io_init(&conn->w_remote_write, connect_cb, conn->sock_remote, EV_WRITE);
			conn->w_remote_write.data = (void *)conn;
			ev_io_start(EV_A_ &conn->w_remote_write);
			conn->state = CMD_RCVD;
			connect(conn->sock_remote, (struct sockaddr *)servers[index].addr, servers[index].addrlen);
		}
		else
		{
			// 命令应答格式
			// +-----+-----+-------+------+----------+----------+
			// | VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
			// +-----+-----+-------+------+----------+----------+
			// |  1  |  1  | X'00' |  1   | Variable |    2     |
			// +-----+-----+-------+------+----------+----------+
			bzero(conn->tx_buf, 10);
			conn->tx_buf[0] = 0x05;
			if (error == 2)
			{
				conn->tx_buf[1] = 0x07;
			}
			else if (error == 3)
			{
				conn->tx_buf[1] = 0x08;
			}
			else
			{
				conn->tx_buf[1] = 0x01;
			}
			conn->tx_buf[2] = 0x00;
			conn->tx_buf[3] = 0x01;
			conn->tx_bytes = 10;
			conn->state = CMD_ERR;
			ev_io_start(EV_A_ &conn->w_local_write);
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
		io_encrypt(conn->tx_buf, conn->tx_bytes, &conn->enc_evp);
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
	case NEGO_RCVD:
	case NEGO_ERR:
	{
		ssize_t tx_bytes = send(conn->sock_local, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
		if (tx_bytes != conn->tx_bytes)
		{
			if (tx_bytes < 0)
			{
				ERR("send");
			}
			close(conn->sock_local);
			mem_delete(conn);
			return;
		}
		if (conn->state == NEGO_RCVD)
		{
			conn->state = NEGO_SENT;
			ev_io_start(EV_A_ &conn->w_local_read);
		}
		else
		{
			conn->state = CLOSE_WAIT;
			ev_timer *w_timer = (ev_timer *)mem_new(sizeof(ev_timer));
			if (w_timer == NULL)
			{
				close(conn->sock_local);
				mem_delete(conn);
				return;
			}
			ev_timer_init(w_timer, closewait_cb, 1.0, 0);
			w_timer->data = (void *)conn;
			ev_timer_start(EV_A_ w_timer);
		}
		break;
	}
	case CMD_ERR:
	case REQ_ERR:
	case REP_RCVD:
	{
		ssize_t tx_bytes = send(conn->sock_local, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
		if (tx_bytes != conn->tx_bytes)
		{
			if (tx_bytes < 0)
			{
				ERR("send");
			}
			close(conn->sock_local);
			mem_delete(conn);
			return;
		}
		if (conn->state == REP_RCVD)
		{
			conn->state = ESTAB;
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

	if (conn->state != ESTAB)
	{
		ev_io_stop(EV_A_ w);
	}

	switch (conn->state)
	{
	case REQ_SENT:
	{
		ssize_t rx_bytes = recv(conn->sock_remote, conn->rx_buf, BUF_SIZE, 0);
		if (rx_bytes != 4)
		{
			if (conn->rx_bytes < 0)
			{
#ifndef NDEBUG
				ERR("recv");
#endif
				LOG("server reset");
			}
			cleanup(EV_A_ conn);
			return;
		}
		// iosocks 应答
		// +-------+
		// | MAGIC |
		// +-------+
		// |   4   |
		// +-------+
		io_decrypt(conn->rx_buf, rx_bytes, &conn->enc_evp);
		// 命令应答格式
		// +----+-----+-------+------+----------+----------+
		// |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
		// +----+-----+-------+------+----------+----------+
		// | 1  |  1  | X'00' |  1   | Variable |    2     |
		// +----+-----+-------+------+----------+----------+
		bzero(conn->tx_buf, 10);
		conn->tx_buf[0] = 0x05;
		conn->tx_buf[1] = 0x00;
		conn->tx_buf[2] = 0x00;
		conn->tx_buf[3] = 0x01;
		conn->tx_bytes = 10;
		conn->state = REP_RCVD;
		uint32_t magic = ntohl(*((uint32_t *)(conn->rx_buf)));
		if (magic != MAGIC)
		{
			LOG("connect failed");
			conn->state = REQ_ERR;
			conn->tx_buf[1] = 0x05;
		}
		ev_io_start(EV_A_ &conn->w_local_write);
		break;
	}
	case ESTAB:
	{
		conn->rx_bytes = recv(conn->sock_remote, conn->rx_buf, BUF_SIZE, 0);
		if (conn->rx_bytes <= 0)
		{
			if (conn->rx_bytes < 0)
			{
#ifndef NDEBUG
				ERR("recv");
#endif
				LOG("server reset");
			}
			cleanup(EV_A_ conn);
			return;
		}
		io_decrypt(conn->rx_buf, conn->rx_bytes, &conn->enc_evp);
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

static void remote_write_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);

	if (conn->state != ESTAB)
	{
		ev_io_stop(EV_A_ w);
	}

	switch (conn->state)
	{
	case CONNECTED:
	{
		ssize_t tx_bytes = send(conn->sock_remote, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
		if (tx_bytes != conn->tx_bytes)
		{
			cleanup(EV_A_ conn);
			return;
		}
		conn->state = REQ_SENT;
		ev_io_init(&conn->w_remote_read, remote_read_cb, conn->sock_remote, EV_READ);
		conn->w_remote_read.data = (void *)conn;
		ev_io_start(EV_A_ &conn->w_remote_read);
		break;
	}
	case ESTAB:
	{
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

static void connect_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);
	assert(conn->state == CMD_RCVD);

	ev_io_stop(EV_A_ w);

	int error = 0;
	socklen_t len = sizeof(int);
	getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &error, &len);
	if (error == 0)
	{
		conn->state = CONNECTED;
		ev_io_init(&conn->w_remote_write, remote_write_cb, conn->sock_remote, EV_WRITE);
		conn->w_remote_write.data = conn;
		ev_io_start(EV_A_ &conn->w_remote_write);
	}
	else
	{
		LOG("connect to iosocks server failed");
		// 命令应答格式
		// +-----+-----+-------+------+----------+----------+
		// | VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
		// +-----+-----+-------+------+----------+----------+
		// |  1  |  1  | X'00' |  1   | Variable |    2     |
		// +-----+-----+-------+------+----------+----------+
		bzero(conn->tx_buf, 10);
		conn->tx_buf[0] = 0x05;
		conn->tx_buf[1] = 0x05;
		conn->tx_buf[2] = 0x00;
		conn->tx_buf[3] = 0x01;
		conn->tx_bytes = 10;
		conn->state = REQ_ERR;
		close(conn->sock_remote);
		ev_io_start(EV_A_ &conn->w_local_write);
	}
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

static void rand_bytes(void *stream, size_t len)
{
	static int urand = -1;
	if (urand == -1)
	{
		urand = open("/dev/urandom", O_RDONLY, 0);
	}
	read(urand, stream, len);
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
