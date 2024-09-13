#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_ENTRIES 10

/* You won't lose style points for including this long line in your code */
static const char *USER_AGENT_HEADER = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* 函数声明 */
void handle_client_request(int client_fd);
int parse_uri(const char *uri, char *hostname, char *path, char *port, char *request_header);
void read_and_forward_headers(rio_t *rp, int server_fd);
void send_content_to_client(int server_fd, int client_fd, const char *uri);
void *client_thread(void *client_fd_ptr);
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
struct CacheEntry cache[MAX_CACHE_ENTRIES]; // 缓存数组，最多有 MAX_CACHE_ENTRIES 个条目
struct RWLock *rw_lock;                      // 读写锁指针

void initialize_rw_lock();                    // 初始化读写锁
char *retrieve_from_cache(const char *url);   // 从缓存中读取内容
void store_in_cache(const char *content, const char *url); // 写入缓存

int main(int argc, char **argv)
{
    int listen_fd;
    int *conn_fd;           /* 使用指针以避免竞争条件 */
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

        /* 等待来自客户端的连接请求到监听描述符 listen_fd，
           然后在 client_addr 中填写客户端的套接字地址，并返回一个已连接描述符 */
        *conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

        /* 将套接字地址结构 client_addr 转化成对应的主机和服务名字符串，
           并将它们复制到 hostname 和 port 缓冲区 */
        Getnameinfo((SA *)&client_addr, client_len, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        /* 调用 pthread_create 函数来创建线程 */
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
        free(cached_content);
    } else {
        /* 解析 URI 以获取主机名、路径和端口，并生成请求头 */
        parse_uri(uri, hostname, path, port, request_header);

        /* 建立与服务器的连接 */
        server_fd = Open_clientfd(hostname, port);

        /* 将请求头传递给服务器 */
        Rio_writen(server_fd, request_header, strlen(request_header));
        read_and_forward_headers(&rio, server_fd);

        /* 将服务端读取的数据返回给客户端 */
        send_content_to_client(server_fd, client_fd, uri);
    }
}

/* 解析 URI 中的主机名、路径和端口，并生成请求头
 * URI 例子：http://www.cmu.edu:8080/hub/index.html
 * 主机名：www.cmu.edu
 * 路径：/hub/index.html
 * 端口：8080
 */
int parse_uri(const char *uri, char *hostname, char *path, char *port, char *request_header)
{
    // 解析 URI 以提取主机名、路径和端口
    sprintf(port, "80"); // 默认端口

    const char *uri_end = uri + strlen(uri); // URI 的最后一个字符，不是 '\0'。

    const char *hostname_start = strstr(uri, "//");

    hostname_start = (hostname_start != NULL ? hostname_start + 2 : uri); // 获取主机名的开始位置。

    const char *end = hostname_start;
    // 获取主机名的结束位置。
    while (*end != '/' && *end != ':') end++;
    strncpy(hostname, hostname_start, end - hostname_start);

    const char *start = end + 1; // 获取端口的开始位置
    if (*end == ':') { // 如果 URI 中有端口
        end++;
        start = strstr(hostname_start, "/"); // 获取端口的结束位置

        strncpy(port, end, start - end);
        end = start; // 获取 URI 的开始位置
    }
    strncpy(path, end, (int)(uri_end - end) + 1);

    /* 请求行：GET /hub/index.html HTTP/1.0 */
    sprintf(request_header, "GET %s HTTP/1.0\r\nHost: %s\r\n", path, hostname);

    return 1;
}


/* 读取 HTTP 请求头，并将其转发到服务器 */
void read_and_forward_headers(rio_t *rp, int server_fd)
{
    char buffer[MAXLINE];

    sprintf(buffer, "%s", USER_AGENT_HEADER);
    Rio_writen(server_fd, buffer, strlen(buffer));
    sprintf(buffer, "Connection: close\r\n");
    Rio_writen(server_fd, buffer, strlen(buffer));
    sprintf(buffer, "Proxy-Connection: close\r\n");
    Rio_writen(server_fd, buffer, strlen(buffer));

    /* 转发其他的请求头 */
    while (Rio_readlineb(rp, buffer, MAXLINE) && strcmp(buffer, "\r\n") != 0) {
        if (strncmp("Host", buffer, 4) == 0 ||
            strncmp("User-Agent", buffer, 10) == 0 ||
            strncmp("Connection", buffer, 10) == 0 ||
            strncmp("Proxy-Connection", buffer, 16) == 0) {
            continue;
        }
        printf("%s", buffer);
        Rio_writen(server_fd, buffer, strlen(buffer));
    }
    Rio_writen(server_fd, buffer, strlen(buffer));
}

/* 从服务器读取数据并返回给客户端，同时写入缓存 */
void send_content_to_client(int server_fd, int client_fd, const char *uri)
{
    size_t bytes_read, total_size = 0;
    char buffer[MAXLINE], content[MAX_OBJECT_SIZE];
    rio_t server_rio;

    Rio_readinitb(&server_rio, server_fd);
    while ((bytes_read = Rio_readlineb(&server_rio, buffer, MAXLINE)) != 0) {
        Rio_writen(client_fd, buffer, bytes_read);

        if (bytes_read + total_size <= MAX_OBJECT_SIZE) {
            sprintf(content + total_size, "%s", buffer);
            total_size += bytes_read;
        } else {
            total_size = MAX_OBJECT_SIZE + 1;
        }
    }

    store_in_cache(content, uri);
}

/*-----缓存操作开始-----*/
void initialize_rw_lock() // 初始化读写锁
{
    rw_lock->read_count = 0;
    sem_init(&rw_lock->read_mutex, 0, 1);
    sem_init(&rw_lock->write_mutex, 0, 1);
}

void store_in_cache(const char *content, const char *url) // 向缓存写入数据
{
    sem_wait(&rw_lock->write_mutex); // 等待获得写者锁
    int index;

    /* 检查缓存是否有空位 */
    for (index = 0; index < MAX_CACHE_ENTRIES; index++) {
        if (cache[index].lru_value == 0) {
            break;
        }
    }

    /* 如果没有空位，根据 LRU 策略驱逐旧条目 */
    if (index == MAX_CACHE_ENTRIES) {
        int min_lru = cache[0].lru_value;

        /* 找到最少被访问的缓存条目 */
        for (int i = 1; i < MAX_CACHE_ENTRIES; i++) {
            if (cache[i].lru_value < min_lru) {
                min_lru = cache[i].lru_value;
                index = i;
            }
        }
    }

    cache[index].lru_value = get_max_lru_value() + 1;
    strcpy(cache[index].url, url);
    strcpy(cache[index].content, content);
    sem_post(&rw_lock->write_mutex); // 释放写者锁
}

char *retrieve_from_cache(const char *url) // 从缓存中读取数据
{
    sem_wait(&rw_lock->read_mutex); // 获取读者锁
    if (rw_lock->read_count == 1) {
        sem_wait(&rw_lock->write_mutex); // 读者在读，不允许有写者
    }
    rw_lock->read_count++;
    sem_post(&rw_lock->read_mutex); // 释放读者锁

    char *content = NULL;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        /* 找到匹配的缓存条目 */
        if (strcmp(url, cache[i].url) == 0) {
            content = (char *)Malloc(strlen(cache[i].content));
            strcpy(content, cache[i].content);
            int max_lru = get_max_lru_value(); // 获取最大的 LRU 值
            cache[i].lru_value = max_lru + 1; // 更新为最大的 LRU 值
            break;
        }
    }

    sem_wait(&rw_lock->read_mutex); // 获取读者锁
    rw_lock->read_count--;
    if (rw_lock->read_count == 0) {
        sem_post(&rw_lock->write_mutex); // 如果没有读者，释放写者锁
    }
    sem_post(&rw_lock->read_mutex); // 释放读者锁
    return content;
}

int get_max_lru_value() // 计算最大的 LRU 值
{
    int max_lru = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (cache[i].lru_value > max_lru) {
            max_lru = cache[i].lru_value;
        }
    }
    return max_lru;
}
