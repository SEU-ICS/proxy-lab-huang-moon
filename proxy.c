#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_ENTRIES 10

/* You won't lose style points for including this long line in your code */
#define USER_AGENT_HEADER "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n"
#define CONNECTION_CLOSE_HEADER "Connection: close\r\n"
#define PROXY_CONNECTION_CLOSE_HEADER "Proxy-Connection: close\r\n"

/* 函数声明 */
void handle_client_request(int client_fd);
int parse_uri(const char *uri, char *hostname, char *path, char *port, char *request_header);
void read_and_forward_headers(rio_t *rp, int server_fd);
void send_content_to_client(int server_fd, int client_fd, const char *uri);
void *client_thread(void *client_fd_ptr);
void send_predefined_headers(int server_fd);
char *retrieve_from_cache(const char *url);
void store_in_cache(const char *content, const char *url);
void initialize_rw_lock();
int get_max_lru_value();

/* 读写者锁的结构体 */
struct RWLock {
    sem_t read_mutex;      // 读写锁的互斥量
    sem_t write_mutex;     // 写锁的互斥量
    int read_count;        // 读者计数
};

/* LRU缓存的结构体 */
struct CacheEntry {
    int lru_value;                 // LRU计数，用于排序
    char url[MAXLINE];             // 缓存的URL
    char content[MAX_OBJECT_SIZE]; // 缓存的内容
};

/* 全局缓存数组和读写锁指针 */
struct CacheEntry cache[MAX_CACHE_ENTRIES];
struct RWLock *rw_lock;

int main(int argc, char **argv)
{
    int listen_fd;
    int *conn_fd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t client_len;
    struct sockaddr_storage client_addr;
    pthread_t thread_id;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    rw_lock = Malloc(sizeof(struct RWLock));
    initialize_rw_lock();

    /* 代理创建一个监听描述符，准备好接收连接请求 */
    listen_fd = Open_listenfd(argv[1]);
    while (1) {
        client_len = sizeof(client_addr);
        conn_fd = Malloc(sizeof(int));

        *conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);
        Getnameinfo((SA *)&client_addr, client_len, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        Pthread_create(&thread_id, NULL, client_thread, conn_fd);
    }
}

void *client_thread(void *client_fd_ptr)
{
    int client_fd = *((int *)client_fd_ptr);
    Pthread_detach(pthread_self());
    Free(client_fd_ptr);
    handle_client_request(client_fd);
    Close(client_fd);
    return NULL;
}

/* 处理客户端HTTP事务 */
void handle_client_request(int client_fd)
{
    char buffer[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE], port[MAXLINE], request_header[MAXLINE];
    int server_fd;
    rio_t rio;

    /* 读取请求行和请求头 */
    Rio_readinitb(&rio, client_fd);
    Rio_readlineb(&rio, buffer, MAXLINE);
    sscanf(buffer, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        printf("Method not implemented");
        return;
    }

    // 尝试从缓存中读取内容
    char *cached_content = retrieve_from_cache(uri);
    if (cached_content != NULL) {
        Rio_writen(client_fd, cached_content, strlen(cached_content));
        return;
    }

    /* 解析 URI 以获取主机名、路径和端口，并生成请求头 */
    parse_uri(uri, hostname, path, port, request_header);

    /* 建立与服务器的连接 */
    server_fd = Open_clientfd(hostname, port);

    /* 发送预定义的请求头信息 */
    send_predefined_headers(server_fd);
    read_and_forward_headers(&rio, server_fd);

    /* 将服务端读取的数据返回给客户端 */
    send_content_to_client(server_fd, client_fd, uri);

    Close(server_fd);
}

/* 解析 URI 并生成请求头 */
int parse_uri(const char *uri, char *hostname, char *path, char *port, char *request_header)
{
    sprintf(port, "80"); // 默认端口

    const char *hostname_start = strstr(uri, "//");
    hostname_start = (hostname_start != NULL ? hostname_start + 2 : uri);

    const char *end = hostname_start;
    while (*end != '/' && *end != ':') end++;
    strncpy(hostname, hostname_start, end - hostname_start);

    const char *start = end + 1;
    if (*end == ':') {
        end++;
        start = strstr(hostname_start, "/");
        strncpy(port, end, start - end);
        end = start;
    }
    strncpy(path, end, strlen(uri) - (end - uri));

    sprintf(request_header, "GET %s HTTP/1.0\r\nHost: %s\r\n", path, hostname);

    return 1;
}

/* 发送预定义的请求头信息 */
void send_predefined_headers(int server_fd)
{
    char buffer[MAXLINE];
    sprintf(buffer, "%s%s%s", USER_AGENT_HEADER, CONNECTION_CLOSE_HEADER, PROXY_CONNECTION_CLOSE_HEADER);
    Rio_writen(server_fd, buffer, strlen(buffer));
}

/* 读取 HTTP 请求头并转发 */
void read_and_forward_headers(rio_t *rp, int server_fd)
{
    char buffer[MAXLINE];
    while (Rio_readlineb(rp, buffer, MAXLINE) && strcmp(buffer, "\r\n") != 0) {
        if (strncmp("Host", buffer, 4) == 0 ||
            strncmp("User-Agent", buffer, 10) == 0 ||
            strncmp("Connection", buffer, 10) == 0 ||
            strncmp("Proxy-Connection", buffer, 16) == 0) {
            continue;
        }
        Rio_writen(server_fd, buffer, strlen(buffer));
    }
    Rio_writen(server_fd, "\r\n", 2); // 结束头信息
}

/* 发送内容给客户端并缓存 */
void send_content_to_client(int server_fd, int client_fd, const char *uri)
{
    size_t bytes_read, total_size = 0;
    char buffer[MAXLINE], content[MAX_OBJECT_SIZE];
    rio_t server_rio;

    Rio_readinitb(&server_rio, server_fd);
    while ((bytes_read = Rio_readlineb(&server_rio, buffer, MAXLINE)) != 0) {
        Rio_writen(client_fd, buffer, bytes_read);
        if (total_size + bytes_read <= MAX_OBJECT_SIZE) {
            memcpy(content + total_size, buffer, bytes_read);
            total_size += bytes_read;
        }
    }

    if (total_size <= MAX_OBJECT_SIZE) {
        store_in_cache(content, uri);
    }
}

/* 缓存相关函数 */
char *retrieve_from_cache(const char *url) {
    P(&rw_lock->read_mutex);
    rw_lock->read_count++;
    if (rw_lock->read_count == 1) {
        P(&rw_lock->write_mutex);
    }
    V(&rw_lock->read_mutex);

    char *content = NULL;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (strcmp(cache[i].url, url) == 0) {
            content = strdup(cache[i].content);
            cache[i].lru_value = get_max_lru_value() + 1;
            break;
        }
    }

    P(&rw_lock->read_mutex);
    rw_lock->read_count--;
    if (rw_lock->read_count == 0) {
        V(&rw_lock->write_mutex);
    }
    V(&rw_lock->read_mutex);

    return content;
}

void store_in_cache(const char *content, const char *url) {
    P(&rw_lock->write_mutex);

    int min_lru_index = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (cache[i].lru_value < cache[min_lru_index].lru_value) {
            min_lru_index = i;
        }
    }

    strcpy(cache[min_lru_index].url, url);
    memcpy(cache[min_lru_index].content, content, strlen(content) + 1);
    cache[min_lru_index].lru_value = get_max_lru_value() + 1;

    V(&rw_lock->write_mutex);
}

int get_max_lru_value() {
    int max_value = cache[0].lru_value;
    for (int i = 1; i < MAX_CACHE_ENTRIES; i++) {
        if (cache[i].lru_value > max_value) {
            max_value = cache[i].lru_value;
        }
    }
    return max_value;
}

void initialize_rw_lock() {
    rw_lock->read_count = 0;
    sem_init(&rw_lock->read_mutex, 0, 1);
    sem_init(&rw_lock->write_mutex, 0, 1);
}
