#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_CAPACITY 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void process_request(int client_sock);  // 处理客户端请求
int analyze_uri(const char *request_uri, char *server_host, char *file_path, char *server_port, char *http_header); // 解析URI
void handle_and_forward_headers(rio_t *server_read, int client_sock); // 转发请求头
void transmit_data(int src_sock, int dst_sock, const char *cache_uri); // 转发数据并写入缓存
void *worker_routine(void *client_sock_ptr); // 处理每个客户端连接的线程
int find_highest_lru(); // 查找最大LRU值

/* 读写锁 */
struct LockRW {
    sem_t read_lock;    
    sem_t write_lock;   
    int reader_counter;
};

/* LRU缓存 */
struct CacheItem {
    int lru_count;                    
    char uri_data[MAXLINE];           
    char data_block[MAX_OBJECT_SIZE];  
};

/* 全局缓存数组和锁 */
struct CacheItem cache_store[CACHE_CAPACITY]; 
struct LockRW *lock_rw;

/* 函数声明 */
void init_rw_lock(); 
char *get_from_cache(const char *uri_req); 
void save_to_cache(const char *data, const char *uri_req); 

int main(int argc, char **argv)
{
    int listen_socket;
    int *conn_socket_ptr;
    char client_host[MAXLINE], client_port[MAXLINE];
    socklen_t client_addr_len;
    struct sockaddr_storage client_socket_addr;
    pthread_t thread_handle;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    lock_rw = Malloc(sizeof(struct LockRW)); 
    init_rw_lock();

    /* 创建监听socket */
    listen_socket = Open_listenfd(argv[1]); 
    while (1) {
        client_addr_len = sizeof(client_socket_addr);

        conn_socket_ptr = Malloc(sizeof(int)); 
        *conn_socket_ptr = Accept(listen_socket, (SA *)&client_socket_addr, &client_addr_len);
        /* 获取客户端主机名和端口号 */
        Getnameinfo((SA *)&client_socket_addr, client_addr_len, client_host, MAXLINE, client_port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", client_host, client_port);
        /* 创建新线程处理客户端请求 */
        Pthread_create(&thread_handle, NULL, worker_routine, conn_socket_ptr);
    }
}

/* 每个客户端连接对应的工作线程 */
void *worker_routine(void *client_sock_ptr)
{
    int client_socket = *((int *)client_sock_ptr);
    Pthread_detach(pthread_self());
    Free(client_sock_ptr); 
    process_request(client_socket); 
    Close(client_socket); 
    return NULL;
}

/* 处理客户端HTTP请求 */
void process_request(int client_socket)
{
    char req_buffer[MAXLINE], req_method[MAXLINE], req_uri[MAXLINE], req_ver[MAXLINE];
    char req_host[MAXLINE], req_path[MAXLINE], req_port[MAXLINE], http_header[MAXLINE];
    int server_socket;
    rio_t client_rio;

    /* 初始化客户端的RIO缓冲区并读取请求行 */
    Rio_readinitb(&client_rio, client_socket);
    Rio_readlineb(&client_rio, req_buffer, MAXLINE); 
    sscanf(req_buffer, "%s %s %s", req_method, req_uri, req_ver); 

    /* 只支持GET方法 */
    if (strcasecmp(req_method, "GET")) {
        printf("Method not implemented");
        return;
    }

    /* 尝试从缓存中获取数据 */
    char *cached_data = get_from_cache(req_uri);
    if (cached_data != NULL) {
        Rio_writen(client_socket, cached_data, strlen(cached_data)); 
        free(cached_data);
    } else {
        analyze_uri(req_uri, req_host, req_path, req_port, http_header);
        server_socket = Open_clientfd(req_host, req_port); 
        Rio_writen(server_socket, http_header, strlen(http_header));
        handle_and_forward_headers(&client_rio, server_socket); 
        transmit_data(server_socket, client_socket, req_uri);
    }
}

/* 解析请求的URI以获取服务器主机名、路径和端口号 */
int analyze_uri(const char *request_uri, char *server_host, char *file_path, char *server_port, char *http_header)
{
    sprintf(server_port, "80");

    const char *uri_tail = request_uri + strlen(request_uri); 
    const char *host_start = strstr(request_uri, "//");

    host_start = (host_start != NULL ? host_start + 2 : request_uri); // 确定主机部分的开始位置

    const char *host_end = host_start;
    while (*host_end != '/' && *host_end != ':') host_end++; // 找到主机部分的结束位置
    strncpy(server_host, host_start, host_end - host_start);

    const char *port_or_path = host_end + 1; 
    if (*host_end == ':') { 
        host_end++;
        port_or_path = strstr(host_start, "/"); 
        strncpy(server_port, host_end, port_or_path - host_end); 
        host_end = port_or_path;
    }
    strncpy(file_path, host_end, (int)(uri_tail - host_end) + 1);

    /* 生成HTTP请求头 */
    sprintf(http_header, "GET %s HTTP/1.0\r\nHost: %s\r\n", file_path, server_host);

    return 1;
}

/* 转发客户端的HTTP请求头到服务器 */
void handle_and_forward_headers(rio_t *server_read, int client_sock)
{
    char header_buffer[MAXLINE];

    sprintf(header_buffer, "%s", user_agent_hdr); // 转发User-Agent头
    Rio_writen(client_sock, header_buffer, strlen(header_buffer));
    sprintf(header_buffer, "Connection: close\r\n"); 
    Rio_writen(client_sock, header_buffer, strlen(header_buffer));
    sprintf(header_buffer, "Proxy-Connection: close\r\n");
    Rio_writen(client_sock, header_buffer, strlen(header_buffer));

    /* 转发除特定头以外的其他请求头 */
    while (Rio_readlineb(server_read, header_buffer, MAXLINE) && strcmp(header_buffer, "\r\n") != 0) {
        if (strncmp("Host", header_buffer, 4) == 0 ||
            strncmp("User-Agent", header_buffer, 10) == 0 ||
            strncmp("Connection", header_buffer, 10) == 0 ||
            strncmp("Proxy-Connection", header_buffer, 16) == 0) {
            continue;
        }
        printf("%s", header_buffer);
        Rio_writen(client_sock, header_buffer, strlen(header_buffer));
    }
    Rio_writen(client_sock, header_buffer, strlen(header_buffer)); 
}

/* 从服务器获取数据并转发给客户端，同时将数据写入缓存 */
void transmit_data(int server_sock, int client_sock, const char *cache_uri)
{
    size_t read_size, total_bytes = 0;
    char transfer_buffer[MAXLINE], content_buffer[MAX_OBJECT_SIZE];
    rio_t server_rio;

    Rio_readinitb(&server_rio, server_sock);
    while ((read_size = Rio_readlineb(&server_rio, transfer_buffer, MAXLINE)) != 0) {
        Rio_writen(client_sock, transfer_buffer, read_size);

        /* 如果数据大小适合缓存，则将其保存到缓存中 */
        if (read_size + total_bytes <= MAX_OBJECT_SIZE) {
            sprintf(content_buffer + total_bytes, "%s", transfer_buffer);
            total_bytes += read_size;
        } else {
            total_bytes = MAX_OBJECT_SIZE + 1; 
        }
    }

    save_to_cache(content_buffer, cache_uri);
}

/*-----缓存操作-----*/

/* 初始化读写锁 */
void init_rw_lock() 
{
    lock_rw->reader_counter = 0; 
    sem_init(&lock_rw->read_lock, 0, 1); 
    sem_init(&lock_rw->write_lock, 0, 1);
}

/* 将数据保存到缓存 */
void save_to_cache(const char *data, const char *uri_req)
{
    sem_wait(&lock_rw->write_lock); 
    int slot;

    /* 查找可用的缓存槽位 */
    for (slot = 0; slot < CACHE_CAPACITY; slot++) {
        if (cache_store[slot].lru_count == 0) { 
            break;
        }
    }

    /* 如果缓存已满，找到最少使用的条目进行替换 */
    if (slot == CACHE_CAPACITY) {
        int min_lru = cache_store[0].lru_count;
        for (int i = 1; i < CACHE_CAPACITY; i++) {
            if (cache_store[i].lru_count < min_lru) {
                min_lru = cache_store[i].lru_count;
                slot = i;
            }
        }
    }

    /* 将数据保存到缓存 */
    cache_store[slot].lru_count = find_highest_lru() + 1; 
    strcpy(cache_store[slot].uri_data, uri_req); 
    strcpy(cache_store[slot].data_block, data); 
    sem_post(&lock_rw->write_lock); 
}

/* 查找当前缓存中最高的LRU值 */
int find_highest_lru() 
{
    int max_lru = 0;
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        if (cache_store[i].lru_count > max_lru) {
            max_lru = cache_store[i].lru_count;
        }
    }
    return max_lru;
}

/* 从缓存中获取数据 */
char *get_from_cache(const char *uri_req)
{
    sem_wait(&lock_rw->read_lock);
    if (lock_rw->reader_counter == 1) {
        sem_wait(&lock_rw->write_lock); 
    }
    lock_rw->reader_counter++;
    sem_post(&lock_rw->read_lock);

    char *cached_result = NULL;
    /* 查找缓存中的数据 */
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        if (strcmp(uri_req, cache_store[i].uri_data) == 0) {
            cached_result = (char *)Malloc(strlen(cache_store[i].data_block));
            strcpy(cached_result, cache_store[i].data_block);
            cache_store[i].lru_count = find_highest_lru() + 1; 
            break;
        }
    }

    sem_wait(&lock_rw->read_lock); 
    lock_rw->reader_counter--;
    if (lock_rw->reader_counter == 0) {
        sem_post(&lock_rw->write_lock); 
    }
    sem_post(&lock_rw->read_lock); 
    return cached_result;
}
