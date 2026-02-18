#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define MAX_BUF_SIZE (1024 * 1024)   /* 接收缓冲区 1MB */
#define TIME_SUB_MS(tv1, tv2) \
    ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

/* 安全发送 */
void send_all(int fd, const void *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n <= 0) {
            perror("send");
            exit(1);
        }
        buf = (char *)buf + n;
        len -= n;
    }
}

/* 安全接收（不阻塞） */
int recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, (char *)buf + total, len - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}

/* 连接服务器 */
int connect_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------- RESP 编码函数 ---------- */
static int resp_encode_hset(char *buf, int idx) {
    char key[32], val[32];
    snprintf(key, sizeof(key), "Teacher%d", idx);
    snprintf(val, sizeof(val), "King%d", idx);
    return sprintf(buf,
                   "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                   strlen(key), key, strlen(val), val);
}

static int resp_encode_hget(char *buf, int idx) {
    char key[32];
    snprintf(key, sizeof(key), "Teacher%d", idx);
    return sprintf(buf, "*2\r\n$4\r\nHGET\r\n$%zu\r\n%s\r\n",
                   strlen(key), key);
}

static int resp_encode_hmod(char *buf, int idx) {
    char key[32], val[32];
    snprintf(key, sizeof(key), "Teacher%d", idx);
    snprintf(val, sizeof(val), "Queen%d", idx);
    return sprintf(buf,
                   "*3\r\n$4\r\nHMOD\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                   strlen(key), key, strlen(val), val);
}

/* ---------- 性能测试（无验证，只计数） ---------- */
void benchmark_pipeline(int fd, int count) {
    struct timeval start, end;
    int total_cmds = count * 3;   /* HSET + HGET + HMOD */

    /* 构造所有命令到一个大缓冲区 */
    size_t cmd_size = count * 70 + count * 50 + count * 70; /* 预估 */
    char *cmds = malloc(cmd_size);
    if (!cmds) {
        perror("malloc");
        return;
    }
    int pos = 0;

    /* HSET 阶段 */
    for (int i = 0; i < count; i++)
        pos += resp_encode_hset(cmds + pos, i);
    /* HGET 阶段 */
    for (int i = 0; i < count; i++)
        pos += resp_encode_hget(cmds + pos, i);
    /* HMOD 阶段 */
    for (int i = 0; i < count; i++)
        pos += resp_encode_hmod(cmds + pos, i);

    gettimeofday(&start, NULL);
    send_all(fd, cmds, pos);
    free(cmds);

    /* 接收所有响应，按行计数 */
    int received = 0;
    char recv_buf[MAX_BUF_SIZE];
    int buf_len = 0;

    while (received < total_cmds) {
        int n = recv(fd, recv_buf + buf_len, sizeof(recv_buf) - buf_len, 0);
        if (n <= 0) {
            perror("recv");
            break;
        }
        buf_len += n;
        char *p = recv_buf;
        int remaining = buf_len;
        while (remaining > 0 && received < total_cmds) {
            char *line_end = memchr(p, '\n', remaining);
            if (!line_end) break;          /* 半包，等待更多数据 */
            int line_len = line_end - p + 1;
            received++;
            p += line_len;
            remaining -= line_len;
        }
        if (remaining > 0 && p != recv_buf) {
            memmove(recv_buf, p, remaining);
        }
        buf_len = remaining;
    }

    gettimeofday(&end, NULL);
    int ms = TIME_SUB_MS(end, start);
    double qps = total_cmds * 1000.0 / ms;
    printf("Count: %d, Time: %d ms, QPS: %.0f\n", total_cmds, ms, qps);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <port> <count>\n", argv[0]);
        fprintf(stderr, "  count : number of keys per command (total ops = count*3)\n");
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int count = atoi(argv[3]);
    if (count <= 0) {
        fprintf(stderr, "count must be positive\n");
        return 1;
    }

    int fd = connect_server(ip, port);
    if (fd < 0) return 1;

    benchmark_pipeline(fd, count);

    close(fd);
    return 0;
}
