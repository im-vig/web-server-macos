#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdlib>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <poll.h>
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
std::mutex g_conn_mtx;

struct ClientConn {
    Buffer buff;
    HttpParser parser;
    ClientConn() : buff(4096), parser() {}
};

std::unordered_map<int, std::shared_ptr<ClientConn>> g_conn_ctx;

void handle_sig(int sig) {
    if (sig == SIGINT) {
        g_is_running = false; // 捕获 Ctrl+C，准备退出循环
    }
}

const string SRC_DIR = "./resources"; // 静态资源根目录

std::string getEnvOrDefault(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                return -1;
            };
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::unordered_map<std::string, std::string> parseFormUrlEncoded(const std::string& body) {
    std::unordered_map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        std::string pair = body.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            kv[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        } else if (!pair.empty()) {
            kv[urlDecode(pair)] = "";
        }
        if (amp == body.size()) break;
        pos = amp + 1;
    }
    return kv;
}

std::string buildResultPage(const std::string& title, const std::string& message,
                            const std::string& linkHref, const std::string& linkText) {
    return "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<title>" + title + "</title>"
           "<style>body{font-family:\"Avenir Next\",\"PingFang SC\",sans-serif;background:#f5f7fb;"
           "margin:0;display:grid;place-items:center;min-height:100vh;padding:20px}"
           ".box{width:min(460px,100%);background:#fff;border-radius:14px;padding:24px;"
           "box-shadow:0 16px 40px rgba(24,39,75,.16)}h1{margin:0 0 10px;font-size:28px}"
           "p{margin:0 0 18px;color:#5d6778;font-size:15px}a{display:inline-block;"
           "padding:10px 16px;border-radius:10px;background:#1f58d8;color:#fff;text-decoration:none;"
           "font-weight:700}</style></head><body><main class=\"box\"><h1>" + title + "</h1><p>" + message +
           "</p><a href=\"" + linkHref + "\">" + linkText + "</a></main></body></html>";
}

int getEnvIntOrDefault(const char* key, int fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

void addConnCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_conn_mtx);
    g_conn_ctx[fd] = std::make_shared<ClientConn>();
}

std::shared_ptr<ClientConn> getConnCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_conn_mtx);
    auto it = g_conn_ctx.find(fd);
    if (it == g_conn_ctx.end()) return nullptr;
    return it->second;
}

void removeConnCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_conn_mtx);
    g_conn_ctx.erase(fd);
}

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

bool writeAll(int fd, const std::string& header, const char* fileData, size_t fileSize) {
    const char* hPtr = header.data();
    size_t hLeft = header.size();
    const char* fPtr = fileData;
    size_t fLeft = fileSize;

    while (hLeft > 0 || fLeft > 0) {
        struct iovec iov[2];
        int iovCnt = 0;
        if (hLeft > 0) {
            iov[iovCnt].iov_base = (void*)hPtr;
            iov[iovCnt].iov_len = hLeft;
            iovCnt++;
        }
        if (fLeft > 0) {
            iov[iovCnt].iov_base = (void*)fPtr;
            iov[iovCnt].iov_len = fLeft;
            iovCnt++;
        }

        ssize_t n = writev(fd, iov, iovCnt);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;

                int pr = poll(&pfd, 1, 2000);
                if (pr <= 0) return false;
                continue;
            }
            return false;
        }
        if (n == 0) return false;

        size_t wrote = static_cast<size_t>(n);
        if (hLeft > 0) {
            size_t hWrote = std::min(hLeft, wrote);
            hPtr += hWrote;
            hLeft -= hWrote;
            wrote -= hWrote;
        }
        if (wrote > 0 && fLeft > 0) {
            size_t fWrote = std::min(fLeft, wrote);
            fPtr += fWrote;
            fLeft -= fWrote;
        }
    }
    return true;
}

// ================= 核心业务处理 =================

void handleClient(int clientFd, Poller* poller) {
    auto conn = getConnCtx(clientFd);
    if (!conn) {
        close(clientFd);
        return;
    }

    HttpResponse response;

    // 1. ET 模式下循环读取请求直到 EAGAIN
    while (true) {
        int saveErrno = 0;
        ssize_t n = conn->buff.readFd(clientFd, &saveErrno);
        if (n > 0) continue;
        if (n == 0) {
            close(clientFd);
            removeConnCtx(clientFd);
            return;
        }
        if (saveErrno == EINTR) continue;
        if (saveErrno == EAGAIN || saveErrno == EWOULDBLOCK) break;
        close(clientFd);
        removeConnCtx(clientFd);
        return;
    }

    // 2. 状态机解析请求
    HttpParser::PARSE_RESULT parseResult = conn->parser.parse(conn->buff);
    if (parseResult == HttpParser::PARSE_ERROR) {
        close(clientFd);
        removeConnCtx(clientFd);
        return;
    }
    if (parseResult == HttpParser::PARSE_AGAIN) {
        if (!poller->modFd(clientFd, true)) {
            close(clientFd);
            removeConnCtx(clientFd);
        }
        return;
    }

    string header, body;
    string path = conn->parser.path();
    if(path == "/") path = "/index.html";

    std::string method = conn->parser.method();

    // 3. 业务路由：动态登录 vs 注册 vs 静态文件
    if (path == "/login") {
        if (method == "GET") {
            response.init(SRC_DIR, std::string("/login.html"), conn->parser.isKeepAlive());
            response.makeResponse(header, body);
            if (!writeAll(clientFd, header, response.file(), response.fileSize())) {
                close(clientFd);
                removeConnCtx(clientFd);
                return;
            }
        } else if (method == "POST") {
            std::unordered_map<std::string, std::string> form = parseFormUrlEncoded(conn->parser.body());
            std::string username = form.count("username") ? form["username"] : "";
            std::string password = form.count("password") ? form["password"] : "";
            bool loginSuccess = false;
            if (!username.empty() && !password.empty()) {
                loginSuccess = SqlConnPool::Instance()->CheckUserPassword(username, password);
            }

            body = loginSuccess ? "<html><body><h1>Login Success!</h1></body></html>"
                                : "<html><body><h1>Login Failed!</h1></body></html>";
            header = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
            header += "Content-Length: " + to_string(body.size()) + "\r\n";
            header += "Connection: " + string(conn->parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";

            if (!writeAll(clientFd, header, body.data(), body.size())) {
                close(clientFd);
                removeConnCtx(clientFd);
                return;
            }
            LOG_INFO("Login Processed for FD[%d], user=%s, Success: %d", clientFd, username.c_str(), loginSuccess);
        } else {
            body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
            header = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/html\r\n";
            header += "Content-Length: " + to_string(body.size()) + "\r\n";
            header += "Connection: close\r\n\r\n";
            writeAll(clientFd, header, body.data(), body.size());
            close(clientFd);
            removeConnCtx(clientFd);
            return;
        }
    }
    else if (path == "/register") {
        if (method == "GET") {
            response.init(SRC_DIR, std::string("/register.html"), conn->parser.isKeepAlive());
            response.makeResponse(header, body);
            if (!writeAll(clientFd, header, response.file(), response.fileSize())) {
                close(clientFd);
                removeConnCtx(clientFd);
                return;
            }
        } else if (method == "POST") {
            std::unordered_map<std::string, std::string> form = parseFormUrlEncoded(conn->parser.body());
            std::string username = form.count("username") ? form["username"] : "";
            std::string password = form.count("password") ? form["password"] : "";
            std::string confirmPassword = form.count("confirm_password") ? form["confirm_password"] : "";

            if (username.empty() || password.empty() || confirmPassword.empty()) {
                body = buildResultPage("注册失败", "账号、密码、确认密码都不能为空。", "/register", "返回注册");
                header = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\n";
                header += "Content-Length: " + to_string(body.size()) + "\r\n";
                header += "Connection: " + string(conn->parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";
                if (!writeAll(clientFd, header, body.data(), body.size())) {
                    close(clientFd);
                    removeConnCtx(clientFd);
                    return;
                }
            } else if (password != confirmPassword) {
                body = buildResultPage("注册失败", "两次输入的密码不一致。", "/register", "返回注册");
                header = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\n";
                header += "Content-Length: " + to_string(body.size()) + "\r\n";
                header += "Connection: " + string(conn->parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";
                if (!writeAll(clientFd, header, body.data(), body.size())) {
                    close(clientFd);
                    removeConnCtx(clientFd);
                    return;
                }
            } else if (SqlConnPool::Instance()->UserExists(username)) {
                body = buildResultPage("注册失败", "账号已存在，请更换后重试。", "/register", "返回注册");
                header = "HTTP/1.1 409 Conflict\r\nContent-Type: text/html\r\n";
                header += "Content-Length: " + to_string(body.size()) + "\r\n";
                header += "Connection: " + string(conn->parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";
                if (!writeAll(clientFd, header, body.data(), body.size())) {
                    close(clientFd);
                    removeConnCtx(clientFd);
                    return;
                }
            } else if (SqlConnPool::Instance()->CreateUser(username, password)) {
                header = "HTTP/1.1 302 Found\r\nLocation: /login\r\n";
                header += "Content-Length: 0\r\n";
                header += "Connection: " + string(conn->parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";
                if (!writeAll(clientFd, header, nullptr, 0)) {
                    close(clientFd);
                    removeConnCtx(clientFd);
                    return;
                }
                LOG_INFO("Register Processed for FD[%d], user=%s, Success: 1", clientFd, username.c_str());
            } else {
                body = buildResultPage("注册失败", "创建账号失败，请稍后再试。", "/register", "返回注册");
                header = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n";
                header += "Content-Length: " + to_string(body.size()) + "\r\n";
                header += "Connection: " + string(conn->parser.isKeepAlive() ? "keep-alive" : "close") + "\r\n\r\n";
                if (!writeAll(clientFd, header, body.data(), body.size())) {
                    close(clientFd);
                    removeConnCtx(clientFd);
                    return;
                }
                LOG_INFO("Register Processed for FD[%d], user=%s, Success: 0", clientFd, username.c_str());
            }
        } else {
            body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
            header = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/html\r\n";
            header += "Content-Length: " + to_string(body.size()) + "\r\n";
            header += "Connection: close\r\n\r\n";
            writeAll(clientFd, header, body.data(), body.size());
            close(clientFd);
            removeConnCtx(clientFd);
            return;
        }
    }
    else {
        response.init(SRC_DIR, path, conn->parser.isKeepAlive());
        response.makeResponse(header, body);
        if (!writeAll(clientFd, header, response.file(), response.fileSize())) {
            close(clientFd);
            removeConnCtx(clientFd);
            return;
        }
    }

    // 4. 长连接维持逻辑
    if (conn->parser.isKeepAlive()) {
        conn->parser.reset();
        conn->buff.retrieve(conn->buff.readableBytes());
        if (!poller->modFd(clientFd, true)) {
            close(clientFd);
            removeConnCtx(clientFd);
        }
    } else {
        close(clientFd);
        removeConnCtx(clientFd);
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
    string dbHost = getEnvOrDefault("WS_DB_HOST", "127.0.0.1");
    int dbPort = getEnvIntOrDefault("WS_DB_PORT", 3306);
    string dbUser = getEnvOrDefault("WS_DB_USER", "root");
    string dbPwd = getEnvOrDefault("WS_DB_PASSWORD", "123456");
    string dbName = getEnvOrDefault("WS_DB_NAME", "webdb");
    int dbPoolSize = getEnvIntOrDefault("WS_DB_POOL_SIZE", 10);
    SqlConnPool::Instance()->Init(dbHost.c_str(), dbPort, dbUser.c_str(), dbPwd.c_str(), dbName.c_str(), dbPoolSize);
    
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
                    addConnCtx(connFd);
                    timer.addTimer(connFd, 20000, [connFd]() {
                        close(connFd);
                        removeConnCtx(connFd);
                    });
                    LOG_INFO("New Connection from %s, FD[%d]", inet_ntoa(client.sin_addr), connFd);
                }
            } 
            else if (events[i].readable) {
                timer.addTimer(fd, 20000, [fd]() {
                    close(fd);
                    removeConnCtx(fd);
                });
                pool.enqueue([fd, &poller] { handleClient(fd, &poller); });
            } 
            else if (events[i].hangup || events[i].error) {
                close(fd);
                timer.delTimer(fd);
                removeConnCtx(fd);
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
