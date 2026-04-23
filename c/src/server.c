#include "rinha.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_EVENTS 256
#define READ_CAPACITY 16384
#define LISTEN_BACKLOG 4096

typedef struct Connection {
    int fd;
    size_t read_len;
    size_t write_pos;
    size_t write_len;
    const char *write_ptr;
    struct Connection *free_next;
    char read_buf[READ_CAPACITY];
} Connection;

static ReferenceSet g_refs;
static int g_use_scalar_search = 0;
static Connection *g_free_connections = NULL;

static const char RESPONSE_READY[] =
    "HTTP/1.1 204 No Content\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char RESPONSE_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char RESPONSE_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char RESPONSE_500[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char RESPONSE_JSON_0[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 35\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0.0}";

static const char RESPONSE_JSON_1[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 35\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0.2}";

static const char RESPONSE_JSON_2[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 35\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0.4}";

static const char RESPONSE_JSON_3[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 36\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":0.6}";

static const char RESPONSE_JSON_4[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 36\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":0.8}";

static const char RESPONSE_JSON_5[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 36\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":1.0}";

static const char *const CLASSIFICATION_RESPONSES[6] = {
    RESPONSE_JSON_0,
    RESPONSE_JSON_1,
    RESPONSE_JSON_2,
    RESPONSE_JSON_3,
    RESPONSE_JSON_4,
    RESPONSE_JSON_5,
};

static const size_t CLASSIFICATION_RESPONSE_LENGTHS[6] = {
    sizeof(RESPONSE_JSON_0) - 1,
    sizeof(RESPONSE_JSON_1) - 1,
    sizeof(RESPONSE_JSON_2) - 1,
    sizeof(RESPONSE_JSON_3) - 1,
    sizeof(RESPONSE_JSON_4) - 1,
    sizeof(RESPONSE_JSON_5) - 1,
};

static const char *env_or_default(const char *key, const char *fallback) {
    const char *value = getenv(key);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static int set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int mkdir_p(const char *path) {
    char tmp[256];
    const size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ensure_socket_parent(const char *socket_path) {
    const char *slash = strrchr(socket_path, '/');
    if (slash == NULL || slash == socket_path) {
        return 0;
    }

    char parent[256];
    const size_t len = (size_t)(slash - socket_path);
    if (len >= sizeof(parent)) {
        return -1;
    }
    memcpy(parent, socket_path, len);
    parent[len] = '\0';
    return mkdir_p(parent);
}

static int bind_unix_listener(const char *path) {
    if (ensure_socket_parent(path) != 0) {
        perror("mkdir socket parent");
        return -1;
    }

    unlink(path);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket unix");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        fprintf(stderr, "UNIX_SOCKET_PATH muito longo\n");
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind unix");
        close(fd);
        return -1;
    }
    chmod(path, 0777);

    if (listen(fd, LISTEN_BACKLOG) != 0 || set_nonblocking(fd) != 0) {
        perror("listen unix");
        close(fd);
        return -1;
    }

    return fd;
}

static int bind_tcp_listener(const char *bind_addr) {
    char host[64];
    char port_text[16];
    const char *separator = strrchr(bind_addr, ':');
    if (separator == NULL || separator == bind_addr || separator[1] == '\0') {
        fprintf(stderr, "bind inválido %s\n", bind_addr);
        return -1;
    }

    const size_t host_len = (size_t)(separator - bind_addr);
    if (host_len >= sizeof(host) || strlen(separator + 1) >= sizeof(port_text)) {
        fprintf(stderr, "bind inválido %s\n", bind_addr);
        return -1;
    }
    memcpy(host, bind_addr, host_len);
    host[host_len] = '\0';
    snprintf(port_text, sizeof(port_text), "%s", separator + 1);

    char *end = NULL;
    const long port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "bind inválido %s\n", bind_addr);
        return -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket tcp");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "host inválido %s\n", host);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind tcp");
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) != 0 || set_nonblocking(fd) != 0) {
        perror("listen tcp");
        close(fd);
        return -1;
    }

    return fd;
}

static void close_connection(int epoll_fd, Connection *conn) {
    if (conn == NULL) {
        return;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    conn->fd = -1;
    conn->read_len = 0;
    conn->write_pos = 0;
    conn->write_len = 0;
    conn->write_ptr = NULL;
    conn->free_next = g_free_connections;
    g_free_connections = conn;
}

static int update_connection_events(int epoll_fd, Connection *conn) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP;
    if (conn->write_pos < conn->write_len) {
        event.events |= EPOLLOUT;
    }
    event.data.ptr = conn;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &event);
}

static const char *find_header_end(const char *buffer, size_t len) {
    if (len < 4) {
        return NULL;
    }
    for (size_t index = 0; index + 3 < len; ++index) {
        if (buffer[index] == '\r' &&
            buffer[index + 1] == '\n' &&
            buffer[index + 2] == '\r' &&
            buffer[index + 3] == '\n') {
            return buffer + index + 4;
        }
    }
    return NULL;
}

static int parse_content_length(const char *headers, size_t header_len, size_t *content_length) {
    *content_length = 0;

    const char *line = memchr(headers, '\n', header_len);
    if (line == NULL) {
        return 0;
    }
    line++;

    const char *end = headers + header_len;
    while (line < end) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        if (next == NULL) {
            next = end;
        }

        size_t len = (size_t)(next - line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            len--;
        }

        static const char key[] = "content-length:";
        if (len >= sizeof(key) - 1 && strncasecmp(line, key, sizeof(key) - 1) == 0) {
            const char *p = line + sizeof(key) - 1;
            while (p < line + len && (*p == ' ' || *p == '\t')) {
                p++;
            }
            size_t value = 0;
            while (p < line + len && *p >= '0' && *p <= '9') {
                value = (value * 10) + (size_t)(*p - '0');
                p++;
            }
            *content_length = value;
            return 1;
        }

        line = next + 1;
    }

    return 0;
}

static int request_matches(const char *headers, size_t header_len, const char *method, const char *path) {
    const size_t method_len = strlen(method);
    const size_t path_len = strlen(path);
    const size_t min_len = method_len + 1 + path_len + 1;
    return header_len >= min_len &&
        memcmp(headers, method, method_len) == 0 &&
        headers[method_len] == ' ' &&
        memcmp(headers + method_len + 1, path, path_len) == 0 &&
        headers[method_len + 1 + path_len] == ' ';
}

static void queue_raw(Connection *conn, const char *data, size_t len) {
    conn->write_ptr = data;
    conn->write_pos = 0;
    conn->write_len = len;
}

static void queue_status(Connection *conn, const char *status) {
    if (strcmp(status, "400 Bad Request") == 0) {
        queue_raw(conn, RESPONSE_400, sizeof(RESPONSE_400) - 1);
    } else if (strcmp(status, "404 Not Found") == 0) {
        queue_raw(conn, RESPONSE_404, sizeof(RESPONSE_404) - 1);
    } else {
        queue_raw(conn, RESPONSE_500, sizeof(RESPONSE_500) - 1);
    }
}

static void queue_classification(Connection *conn, const Classification *classification) {
    uint8_t bucket = classification->fraud_count;
    if (bucket > 5) {
        bucket = 0;
    }
    queue_raw(conn, CLASSIFICATION_RESPONSES[bucket], CLASSIFICATION_RESPONSE_LENGTHS[bucket]);
}

static int process_one_request(Connection *conn, const char *headers, size_t header_len, const char *body, size_t body_len) {
    if (request_matches(headers, header_len, "GET", "/ready")) {
        queue_raw(conn, RESPONSE_READY, sizeof(RESPONSE_READY) - 1);
        return 1;
    }

    if (!request_matches(headers, header_len, "POST", "/fraud-score")) {
        queue_status(conn, "404 Not Found");
        return 1;
    }

    Payload payload;
    if (!parse_payload(body, body_len, &payload)) {
        queue_status(conn, "400 Bad Request");
        return 1;
    }

    Classification classification = {.approved = true, .fraud_count = 0};
    const bool classified = g_use_scalar_search
        ? classifier_classify_scalar(&g_refs, &payload, &classification)
        : classifier_classify(&g_refs, &payload, &classification);
    if (!classified) {
        classification.approved = true;
        classification.fraud_count = 0;
    }

    queue_classification(conn, &classification);
    return 1;
}

static int process_requests(Connection *conn) {
    while (conn->write_len == conn->write_pos) {
        conn->write_len = 0;
        conn->write_pos = 0;

        const char *header_end = find_header_end(conn->read_buf, conn->read_len);
        if (header_end == NULL) {
            return 1;
        }

        const size_t header_len = (size_t)(header_end - conn->read_buf);
        size_t content_length = 0;
        parse_content_length(conn->read_buf, header_len, &content_length);
        if (content_length > READ_CAPACITY || header_len + content_length > READ_CAPACITY) {
            return 0;
        }
        if (conn->read_len < header_len + content_length) {
            return 1;
        }

        const char *body = conn->read_buf + header_len;
        if (!process_one_request(conn, conn->read_buf, header_len, body, content_length)) {
            return 0;
        }

        const size_t consumed = header_len + content_length;
        const size_t remaining = conn->read_len - consumed;
        if (remaining > 0) {
            memmove(conn->read_buf, conn->read_buf + consumed, remaining);
        }
        conn->read_len = remaining;

        if (conn->write_len > conn->write_pos) {
            return 1;
        }
    }

    return 1;
}

static int handle_read(Connection *conn) {
    for (;;) {
        if (conn->read_len == READ_CAPACITY) {
            return 0;
        }

        const ssize_t n = read(conn->fd, conn->read_buf + conn->read_len, READ_CAPACITY - conn->read_len);
        if (n > 0) {
            conn->read_len += (size_t)n;
            continue;
        }
        if (n == 0) {
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return 0;
    }

    return process_requests(conn);
}

static int handle_write(Connection *conn) {
    while (conn->write_pos < conn->write_len) {
        const ssize_t n = write(conn->fd, conn->write_ptr + conn->write_pos, conn->write_len - conn->write_pos);
        if (n > 0) {
            conn->write_pos += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 1;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return 0;
    }

    conn->write_pos = 0;
    conn->write_len = 0;
    conn->write_ptr = NULL;
    return process_requests(conn);
}

static Connection *alloc_connection(void) {
    Connection *conn = g_free_connections;
    if (conn != NULL) {
        g_free_connections = conn->free_next;
    } else {
        conn = (Connection *)malloc(sizeof(Connection));
        if (conn == NULL) {
            return NULL;
        }
    }

    conn->fd = -1;
    conn->read_len = 0;
    conn->write_pos = 0;
    conn->write_len = 0;
    conn->write_ptr = NULL;
    conn->free_next = NULL;
    return conn;
}

static int accept_connections(int epoll_fd, int listen_fd) {
    for (;;) {
        const int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd >= 0) {
            Connection *conn = alloc_connection();
            if (conn == NULL) {
                close(fd);
                continue;
            }
            conn->fd = fd;

            struct epoll_event event;
            memset(&event, 0, sizeof(event));
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.ptr = conn;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
                close(fd);
                free(conn);
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        perror("accept");
        return -1;
    }
}

static int run_event_loop(int listen_fd) {
    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event listen_event;
    memset(&listen_event, 0, sizeof(listen_event));
    listen_event.events = EPOLLIN;
    listen_event.data.ptr = NULL;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) != 0) {
        perror("epoll_ctl listen");
        close(epoll_fd);
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        const int count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            close(epoll_fd);
            return 1;
        }

        for (int index = 0; index < count; ++index) {
            if (events[index].data.ptr == NULL) {
                accept_connections(epoll_fd, listen_fd);
                continue;
            }

            Connection *conn = (Connection *)events[index].data.ptr;
            int keep = 1;
            if ((events[index].events & (EPOLLERR | EPOLLHUP)) != 0) {
                keep = 0;
            }
            if (keep && (events[index].events & (EPOLLIN | EPOLLRDHUP)) != 0) {
                keep = handle_read(conn);
            }
            if (keep && (events[index].events & EPOLLOUT) != 0) {
                keep = handle_write(conn);
            }
            if (keep) {
                if (update_connection_events(epoll_fd, conn) != 0) {
                    keep = 0;
                }
            }
            if (!keep) {
                close_connection(epoll_fd, conn);
            }
        }
    }
}

static int load_references(void) {
    char error[256] = {0};
    const char *references_bin = getenv("REFERENCES_BIN_PATH");
    const char *labels_bin = getenv("LABELS_BIN_PATH");
    const char *references_json = env_or_default("REFERENCES_PATH", "resources/references.json.gz");

    if (references_bin != NULL && references_bin[0] != '\0' && labels_bin != NULL && labels_bin[0] != '\0') {
        if (!refs_load_binary(references_bin, labels_bin, &g_refs, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        return 0;
    }

    if ((references_bin != NULL && references_bin[0] != '\0') || (labels_bin != NULL && labels_bin[0] != '\0')) {
        fprintf(stderr, "REFERENCES_BIN_PATH e LABELS_BIN_PATH devem ser informados juntos\n");
        return 1;
    }

    if (!refs_load_gzip_json(references_json, &g_refs, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *search_mode = getenv("RINHA_SEARCH_MODE");
    g_use_scalar_search = search_mode != NULL && strcmp(search_mode, "scalar") == 0;

    if (load_references() != 0) {
        return 1;
    }

    const char *unix_socket_path = getenv("UNIX_SOCKET_PATH");
    int listen_fd = -1;
    if (unix_socket_path != NULL && unix_socket_path[0] != '\0') {
        listen_fd = bind_unix_listener(unix_socket_path);
    } else {
        listen_fd = bind_tcp_listener(env_or_default("BIND_ADDR", "0.0.0.0:3000"));
    }

    if (listen_fd < 0) {
        refs_free(&g_refs);
        return 1;
    }

    const int exit_code = run_event_loop(listen_fd);
    close(listen_fd);
    refs_free(&g_refs);
    return exit_code;
}
