#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include "Log.h"

class SqlConnPool {
public:
    static SqlConnPool* Instance() {
        static SqlConnPool pool;
        return &pool;
    }

    // 获取一个连接
    MYSQL* GetConn() {
        MYSQL* sql = nullptr;
        if(connQue_.empty()) return nullptr;
        sem_wait(&semId_); // 等待信号量
        {
            std::lock_guard<std::mutex> locker(mtx_);
            sql = connQue_.front();
            connQue_.pop();
        }
        return sql;
    }

    // 释放连接回池子
    void FreeConn(MYSQL* conn) {
        if(!conn) return;
        {
            std::lock_guard<std::mutex> locker(mtx_);
            connQue_.push(conn);
        }
        sem_post(&semId_);
    }

    void Init(const char* host, int port,
              const char* user, const char* pwd, 
              const char* dbName, int connSize) {
        for (int i = 0; i < connSize; i++) {
            MYSQL* sql = nullptr;
            sql = mysql_init(sql);
            if (!sql) LOG_ERROR("MySQL init error!");
            sql = mysql_real_connect(sql, host, user, pwd, dbName, port, nullptr, 0);
            if (!sql) LOG_ERROR("MySQL connect error!");
            connQue_.push(sql);
        }
        sem_init(&semId_, 0, connSize);
    }

private:
    SqlConnPool() {}
    ~SqlConnPool() {
        while(!connQue_.empty()) {
            auto item = connQue_.front();
            connQue_.pop();
            mysql_close(item);
        }
        mysql_library_end();
    }

    std::queue<MYSQL*> connQue_;
    std::mutex mtx_;
    sem_t semId_;
};

// RAII 资源管理：自动获取和释放连接
class SqlConnRAII {
public:
    SqlConnRAII(MYSQL** sql, SqlConnPool* connpool) {
        *sql = connpool->GetConn();
        sql_ = *sql;
        connpool_ = connpool;
    }
    ~SqlConnRAII() { connpool_->FreeConn(sql_); }
private:
    MYSQL* sql_;
    SqlConnPool* connpool_;
};

#endif