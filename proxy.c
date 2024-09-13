#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_ENTRIES 10

/* You won't lose style points for including this long line in your code */
static const char *USER_AGENT_HEADER = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* 函数声明 */
void process_client_request(int client_fd);
int analyze_uri(const char *uri, char *hostname, char *path, char *port, char *req_header);
void relay_request_headers(rio_t *rp, int server_fd);
void fetch_and_forward_content(int server_fd, int client_fd, const char *uri);
void *client_handler_thread(void *client_fd_ptr);
int fetch_max_lru();

/* 读写锁结构 */
struct RWLock {
    sem_t read_lock;
    sem_t write_lock;
    int readers_count;
};

/* 缓存项结构 */
struct CacheEntry {
    int lru_counter;             // LRU计数器
    char url[MAXLINE];           // 缓存URL
    char data[MAX_OBJECT_SIZE];  // 缓存内容
};

/* 全局缓存数组和读写锁 */
struct CacheEntry cache_storage[MAX_CACHE_ENTRIES]; // 缓存数组
struct RWLock *cache_lock;                          // 读写锁

void init_rw_lock();
char *check_cache(const char *url);
void cache_data(const char *data, const char *url);

int main(int argc, char **argv) {
    int listen_fd, *conn_fd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t client_len;
    struct sockaddr_storage client_addr;
    pthread_t thread_id;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    cache_lock = Malloc(sizeof(struct RWLock));
    init_rw_lock();

    listen_fd = Open_listenfd(argv[1]);
    while (1) {
        client_len = sizeof(client_addr);
        conn_fd = Malloc(sizeof(int));
        *conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

        Getnameinfo((SA *)&client_addr, client_len, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        Pthread_create(&thread_id, NULL, client_handler_thread, conn_fd);
    }
}

void *client_handler_thread(void *client_fd_ptr) {
    int client_fd = *((int *)client_fd_ptr);
    Pthread_detach(pthread_self());
    Free(client_fd_ptr);
    process_client_request(client_fd);
    Close(client_fd);
    return NULL;
}

/* 处理客户端请求 */
void process_client_request(int client_fd) {
    char buffer[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE], port[MAXLINE], request_header[MAXLINE];
    int server_fd;
    rio_t rio;

    Rio_readinitb(&rio, client_fd);
    Rio_readlineb(&rio, buffer, MAXLINE);
    sscanf(buffer, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        printf("Method not supported\n");
        return;
    }

    // 优先从缓存中查找
    char *cached_data = check_cache(uri);
    if (cached_data != NULL) {
        Rio_writen(client_fd, cached_data, strlen(cached_data));
        free(cached_data);
    } else {
        // 解析 URI 并构建请求头
        analyze_uri(uri, hostname, path, port, request_header);

        // 与目标服务器建立连接
        server_fd = Open_clientfd(hostname, port);

        // 发送请求头
        Rio_writen(server_fd, request_header, strlen(request_header));
        relay_request_headers(&rio, server_fd);

        // 获取并转发服务器内容
        fetch_and_forward_content(server_fd, client_fd, uri);
    }
}

/* 解析 URI */
int analyze_uri(const char *uri, char *hostname, char *path, char *port, char *req_header) {
    strcpy(port, "80");
    const char *host_start = strstr(uri, "//");
    host_start = host_start != NULL ? host_start + 2 : uri;

    const char *path_start = strchr(host_start, '/');
    if (path_start != NULL) {
        strncpy(hostname, host_start, path_start - host_start);
        strcpy(path, path_start);
    } else {
        strcpy(hostname, host_start);
        strcpy(path, "/");
    }

    sprintf(req_header, "GET %s HTTP/1.0\r\nHost: %s\r\n", path, hostname);
    return 0;
}

/* 转发请求头 */
void relay_request_headers(rio_t *rp, int server_fd) {
    char buffer[MAXLINE];

    sprintf(buffer, "%s", USER_AGENT_HEADER);
    Rio_writen(server_fd, buffer, strlen(buffer));
    sprintf(buffer, "Connection: close\r\n");
    Rio_writen(server_fd, buffer, strlen(buffer));
    sprintf(buffer, "Proxy-Connection: close\r\n");
    Rio_writen(server_fd, buffer, strlen(buffer));

    while (Rio_readlineb(rp, buffer, MAXLINE) > 0 && strcmp(buffer, "\r\n") != 0) {
        if (!strstr(buffer, "Host") && !strstr(buffer, "User-Agent") &&
            !strstr(buffer, "Connection") && !strstr(buffer, "Proxy-Connection")) {
            Rio_writen(server_fd, buffer, strlen(buffer));
        }
    }
    Rio_writen(server_fd, "\r\n", 2);
}

/* 获取服务器内容并返回给客户端，同时缓存 */
void fetch_and_forward_content(int server_fd, int client_fd, const char *uri) {
    size_t n, total = 0;
    char buf[MAXLINE], data[MAX_OBJECT_SIZE];
    rio_t server_rio;

    Rio_readinitb(&server_rio, server_fd);
    while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) > 0) {
        Rio_writen(client_fd, buf, n);
        if (total + n <= MAX_OBJECT_SIZE) {
            memcpy(data + total, buf, n);
            total += n;
        }
    }

    if (total <= MAX_OBJECT_SIZE) {
        cache_data(data, uri);
    }
}

/* 读写锁初始化 */
void init_rw_lock() {
    cache_lock->readers_count = 0;
    sem_init(&cache_lock->read_lock, 0, 1);
    sem_init(&cache_lock->write_lock, 0, 1);
}

/* 检查缓存 */
char *check_cache(const char *url) {
    sem_wait(&cache_lock->read_lock);
    if (cache_lock->readers_count == 0) {
        sem_wait(&cache_lock->write_lock);
    }
    cache_lock->readers_count++;
    sem_post(&cache_lock->read_lock);

    char *content = NULL;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (strcmp(cache_storage[i].url, url) == 0) {
            content = (char *)Malloc(strlen(cache_storage[i].data));
            strcpy(content, cache_storage[i].data);
            cache_storage[i].lru_counter = fetch_max_lru() + 1;
            break;
        }
    }

    sem_wait(&cache_lock->read_lock);
    cache_lock->readers_count--;
    if (cache_lock->readers_count == 0) {
        sem_post(&cache_lock->write_lock);
    }
    sem_post(&cache_lock->read_lock);

    return content;
}

/* 缓存数据 */
void cache_data(const char *data, const char *url) {
    sem_wait(&cache_lock->write_lock);
    int min_index = 0;

    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (cache_storage[i].lru_counter == 0) {
            min_index = i;
            break;
        }
        if (cache_storage[i].lru_counter < cache_storage[min_index].lru_counter) {
            min_index = i;
        }
    }

    cache_storage[min_index].lru_counter = fetch_max_lru() + 1;
    strcpy(cache_storage[min_index].url, url);
    strcpy(cache_storage[min_index].data, data);
    sem_post(&cache_lock->write_lock);
}

/* 获取当前最大LRU值 */
int fetch_max_lru() {
    int max_lru = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (cache_storage[i].lru_counter > max_lru) {
            max_lru = cache_storage[i].lru_counter;
        }
    }
    return max_lru;
}
