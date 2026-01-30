#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <errno.h>
#include <signal.h> // 信号处理头文件

// 引入所有自定义模块
#include "ThreadPool.h"
#include "Log.h"
#include "Timer.h"
#include "Buffer.h"
#include "HttpParser.h"
#include "HttpResponse.h"
#include "SqlConnPool.h"

using namespace std;

// ================= 全局变量与信号处理 =================
bool g_is_running = true;

void handle_sig(int sig) {
    if (sig == SIGINT) {
        g_is_running = false; // 捕获 Ctrl+C，准备退出循环
    }
}

const string SRC_DIR = "./resources"; // 静态资源根目录

// ================= 网络工具函数 =================

int setNonBlocking(int fd) {
    int old = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old | O_NONBLOCK);
    return old;
}

void addFd(int epollFd, int fd, bool oneshot) {
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (oneshot) ev.events |= EPOLLONESHOT;
    epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev);
    setNonBlocking(fd);
}

// ================= 核心业务处理 =================

void handleClient(int clientFd, int epollFd) {
    Buffer buff;
    int saveErrno = 0;
    HttpParser parser;
    HttpResponse response;

    // 1. ET 模式下循环读取请求直到 EAGAIN
    while (true) {
        ssize_t n = buff.readFd(clientFd, &saveErrno);
        if (n <= 0) {
            if (saveErrno == EAGAIN) break;
            close(clientFd); return;
        }
    }

    // 2. 状态机解析请求
    if (!parser.parse(buff) || !parser.isFinish()) {
        close(clientFd); return;
    }

    string header, body;
    string path = parser.path();
    if(path == "/") path = "/index.html";

    // 3. 业务路由：动态登录 vs 静态文件
    if (path == "/login") {
        MYSQL* sql;
        SqlConnRAII(&sql, SqlConnPool::Instance());
        
        if(!sql) {
            LOG_ERROR("Database connection failed!");
            body = "<html><body><h1>500 Error</h1></body></html>";
            header = "HTTP/1.1 500 Internal Error\r\nContent-Length: " + to_string(body.size()) + "\r\n\r\n";
            send(clientFd, (header + body).c_str(), header.size() + body.size(), 0);
            close(clientFd); return;
        }

        char order[256] = {0};
        snprintf(order, 256, "SELECT username FROM user WHERE username='admin' LIMIT 1");
        
        bool loginSuccess = false;
        if(mysql_query(sql, order)) {
            LOG_ERROR("SQL Query Error!");
        } else {
            MYSQL_RES* res = mysql_store_result(sql);
            if(mysql_num_rows(res) > 0) loginSuccess = true;
            mysql_free_result(res);
        }

        body = loginSuccess ? "<html><body><h1>Login Success!</h1></body></html>" 
                            : "<html><body><h1>Login Failed!</h1></body></html>";
        
        header = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
        header += "Content-Length: " + to_string(body.size()) + "\r\n";
        header += "Connection: " + string(parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";
        
        send(clientFd, (header + body).c_str(), header.size() + body.size(), 0);
        LOG_INFO("Login Processed for FD[%d], Success: %d", clientFd, loginSuccess);
    } 
    else {
        response.init(SRC_DIR, path, parser.isKeepAlive());
        response.makeResponse(header, body);

        struct iovec iov[2];
        iov[0].iov_base = (char*)header.c_str();
        iov[0].iov_len = header.size();
        
        if(response.file() && response.fileSize() > 0) {
            iov[1].iov_base = response.file();
            iov[1].iov_len = response.fileSize();
            writev(clientFd, iov, 2);
        } else {
            write(clientFd, iov[0].iov_base, iov[0].iov_len);
        }
    }

    // 4. 长连接维持逻辑
    if (parser.isKeepAlive()) {
        struct epoll_event ev = {0};
        ev.data.fd = clientFd;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
        epoll_ctl(epollFd, EPOLL_CTL_MOD, clientFd, &ev);
    } else {
        close(clientFd);
    }
}

// ================= 主循环 =================

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) port = atoi(argv[1]);

    // 注册信号处理器：优雅退出
    signal(SIGINT, handle_sig);

    // 1. 初始化异步日志
    AsyncLogger::getInstance()->init("./server.log");
    LOG_INFO("========== Server Start (Port: %d) ==========", port);

    // 2. 初始化数据库连接池
    SqlConnPool::Instance()->Init("localhost", 3306, "root", "123456", "webdb", 10);
    
    ThreadPool pool(8);
    TimerManager timer;

    // 3. 网络初始化
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        LOG_ERROR("Bind Error!");
        return -1;
    }
    listen(listenFd, 128);

    int epollFd = epoll_create(1);
    addFd(epollFd, listenFd, false);
    epoll_event events[1024];

    // 4. 事件循环：由 g_is_running 控制
    while (g_is_running) {
        int timeout = timer.getNextTick();
        int n = epoll_wait(epollFd, events, 1024, timeout);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listenFd) {
                struct sockaddr_in client;
                socklen_t len = sizeof(client);
                while (true) {
                    int connFd = accept(listenFd, (struct sockaddr*)&client, &len);
                    if (connFd < 0) break;
                    addFd(epollFd, connFd, true);
                    timer.addTimer(connFd, 20000, [connFd]() { close(connFd); });
                    LOG_INFO("New Connection from %s, FD[%d]", inet_ntoa(client.sin_addr), connFd);
                }
            } 
            else if (events[i].events & EPOLLIN) {
                timer.addTimer(fd, 20000, [fd]() { close(fd); });
                pool.enqueue([fd, epollFd] { handleClient(fd, epollFd); });
            } 
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                close(fd);
                timer.delTimer(fd);
            }
        }
    }

    // 5. 退出循环后的清理工作
    LOG_INFO("========== Server Shutdown Gracefully ==========");
    close(listenFd);
    close(epollFd);
    
    // 手动刷新日志并留给后台线程一点时间写完
    AsyncLogger::getInstance()->flush();
    sleep(1); 
    
    return 0;
}