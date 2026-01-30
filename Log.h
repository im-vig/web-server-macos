#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <vector>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sys/time.h>
#include <stdarg.h>

using namespace std;

class AsyncLogger {
public:
    static AsyncLogger* getInstance() {
        static AsyncLogger instance;
        return &instance;
    }

    void init(const char* fileName, int maxQueueSize = 1000) {
        fp_ = fopen(fileName, "a");
        maxQueueSize_ = maxQueueSize;
        isStop_ = false; // 初始化停止标志
        writeThread_ = thread(&AsyncLogger::flushLogThread, this);
    }

    void writeLog(int level, const char* format, ...) {
        char buf[1024] = {0};
        struct timeval now;
        gettimeofday(&now, NULL);
        time_t t = now.tv_sec;
        struct tm* sys_tm = localtime(&t);
        
        int n = snprintf(buf, 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                         sys_tm->tm_year + 1900, sys_tm->tm_mon + 1, sys_tm->tm_mday,
                         sys_tm->tm_hour, sys_tm->tm_min, sys_tm->tm_sec, now.tv_usec);

        va_list valist;
        va_start(valist, format);
        vsnprintf(buf + n, 1023 - n, format, valist);
        va_end(valist);

        {
            unique_lock<mutex> lock(mtx_);
            if (logQueue_.size() < maxQueueSize_) {
                logQueue_.push(string(buf));
            }
        }
        cond_.notify_one();
    }

    void flush() {
        if(fp_) fflush(fp_);
    }

private:
    AsyncLogger() : fp_(nullptr), maxQueueSize_(0), isStop_(false) {} // 构造函数初始化

    ~AsyncLogger() {
        isStop_ = true; // 告诉后台线程停止
        cond_.notify_all(); // 唤醒可能正在等待的后台线程
        if (writeThread_.joinable()) {
            writeThread_.join();
        }
        if (fp_) {
            fflush(fp_);
            fclose(fp_);
        }
    }

    void flushLogThread() {
        while (true) {
            string logLine;
            {
                unique_lock<mutex> lock(mtx_);
                // 如果队列为空且没有停止，就等待
                while (logQueue_.empty() && !isStop_) {
                    cond_.wait(lock);
                }
                
                // 如果停止了且队列也空了，才彻底退出线程
                if (isStop_ && logQueue_.empty()) {
                    break;
                }

                if (!logQueue_.empty()) {
                    logLine = logQueue_.front();
                    logQueue_.pop();
                }
            }
            
            if (!logLine.empty() && fp_) {
                fputs(logLine.c_str(), fp_);
                fputs("\n", fp_);
                fflush(fp_); // 关键：每写一条就强制刷盘，确保实时看到
            }
        }
    }

    FILE* fp_;
    int maxQueueSize_;
    queue<string> logQueue_;
    mutex mtx_;
    condition_variable cond_;
    thread writeThread_;
    bool isStop_; // <--- 刚才报错就是因为少了这一行声明
};

#define LOG_INFO(format, ...) AsyncLogger::getInstance()->writeLog(1, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) AsyncLogger::getInstance()->writeLog(3, format, ##__VA_ARGS__)

#endif