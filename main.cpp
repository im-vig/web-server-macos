#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <errno.h>
#include <signal.h> // 信号处理头文件

// 引入所有自定义模块
#include "Poller.h"
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

bool addFd(Poller& poller, int fd, bool oneshot) {
    setNonBlocking(fd);
    return poller.addFd(fd, oneshot);
}

// ================= 核心业务处理 =================

void handleClient(int clientFd, Poller* poller) {
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
        bool loginSuccess = SqlConnPool::Instance()->CheckAdminUserExists("admin");

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
        if (!poller->modFd(clientFd, true)) close(clientFd);
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
    signal(SIGPIPE, SIG_IGN);

    // 1. 初始化异步日志
    AsyncLogger::getInstance()->init("./server.log");
    LOG_INFO("========== Server Start (Port: %d) ==========", port);

    // 2. 初始化数据库连接池
    SqlConnPool::Instance()->Init("localhost", 3306, "root", "123456", "webdb", 10);
    
    Poller poller;
    if (!poller.isValid()) {
        LOG_ERROR("Poller init error!");
        return -1;
    }
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

    if (::bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        LOG_ERROR("Bind Error!");
        return -1;
    }
    listen(listenFd, 128);

    if (!addFd(poller, listenFd, false)) {
        LOG_ERROR("Add listen fd to poller failed!");
        close(listenFd);
        return -1;
    }
    vector<PollerEvent> events;

    // 4. 事件循环：由 g_is_running 控制
    while (g_is_running) {
        int timeout = timer.getNextTick();
        int n = poller.wait(timeout, events);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Poller wait error!");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].fd;
            if (fd == listenFd && events[i].readable) {
                while (true) {
                    struct sockaddr_in client;
                    socklen_t len = sizeof(client);
                    int connFd = accept(listenFd, (struct sockaddr*)&client, &len);
                    if (connFd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG_ERROR("Accept Error!");
                        break;
                    }
                    if (!addFd(poller, connFd, true)) {
                        close(connFd);
                        continue;
                    }
                    timer.addTimer(connFd, 20000, [connFd]() { close(connFd); });
                    LOG_INFO("New Connection from %s, FD[%d]", inet_ntoa(client.sin_addr), connFd);
                }
            } 
            else if (events[i].readable) {
                timer.addTimer(fd, 20000, [fd]() { close(fd); });
                pool.enqueue([fd, &poller] { handleClient(fd, &poller); });
            } 
            else if (events[i].hangup || events[i].error) {
                close(fd);
                timer.delTimer(fd);
            }
        }
    }

    // 5. 退出循环后的清理工作
    LOG_INFO("========== Server Shutdown Gracefully ==========");
    close(listenFd);
    
    // 手动刷新日志并留给后台线程一点时间写完
    AsyncLogger::getInstance()->flush();
    sleep(1); 
    
    return 0;
}
