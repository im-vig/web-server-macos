#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <string>
#include <queue>
#include <mutex>
#include "Log.h"

#if defined(SQL_CONNPOOL_DISABLE_MYSQL)
#define SQL_CONNPOOL_HAS_MYSQL 0
struct MYSQL;
struct MYSQL_RES;
#elif __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#define SQL_CONNPOOL_HAS_MYSQL 1
#elif __has_include(<mysql.h>)
#include <mysql.h>
#define SQL_CONNPOOL_HAS_MYSQL 1
#else
#define SQL_CONNPOOL_HAS_MYSQL 0
struct MYSQL;
struct MYSQL_RES;
#endif

class SqlConnPool {
public:
    static SqlConnPool* Instance() {
        static SqlConnPool pool;
        return &pool;
    }

    // 获取一个连接
    MYSQL* GetConn() {
        std::lock_guard<std::mutex> locker(mtx_);
        if(connQue_.empty()) return nullptr;
        MYSQL* sql = connQue_.front();
        connQue_.pop();
        return sql;
    }

    // 释放连接回池子
    void FreeConn(MYSQL* conn) {
        if(!conn) return;
        {
            std::lock_guard<std::mutex> locker(mtx_);
            connQue_.push(conn);
        }
    }

    void Init(const char* host, int port,
              const char* user, const char* pwd, 
              const char* dbName, int connSize) {
#if SQL_CONNPOOL_HAS_MYSQL
        int successCnt = 0;
        for (int i = 0; i < connSize; i++) {
            MYSQL* sql = nullptr;
            sql = mysql_init(sql);
            if (!sql) {
                LOG_ERROR("MySQL init error!");
                continue;
            }
            sql = mysql_real_connect(sql, host, user, pwd, dbName, port, nullptr, 0);
            if (!sql) {
                LOG_ERROR("MySQL connect error!");
                mysql_close(sql);
                continue;
            }
            connQue_.push(sql);
            successCnt++;
        }
        if (successCnt == 0) LOG_ERROR("No MySQL connection is available.");
#else
        (void)host;
        (void)port;
        (void)user;
        (void)pwd;
        (void)dbName;
        (void)connSize;
        LOG_ERROR("MySQL support is disabled at build time.");
#endif
    }

    bool CheckAdminUserExists(const std::string& username) {
#if SQL_CONNPOOL_HAS_MYSQL
        MYSQL* sql = GetConn();
        if (!sql) return false;

        std::string query = "SELECT username FROM user WHERE username='" + username + "' LIMIT 1";
        bool exists = false;

        if (mysql_query(sql, query.c_str())) {
            LOG_ERROR("SQL Query Error!");
        } else {
            MYSQL_RES* res = mysql_store_result(sql);
            if (res) {
                exists = (mysql_num_rows(res) > 0);
                mysql_free_result(res);
            }
        }
        FreeConn(sql);
        return exists;
#else
        (void)username;
        return false;
#endif
    }

    bool CheckUserPassword(const std::string& username, const std::string& password) {
#if SQL_CONNPOOL_HAS_MYSQL
        MYSQL* sql = GetConn();
        if (!sql) return false;

        std::string userEsc(username.size() * 2 + 1, '\0');
        unsigned long userLen = mysql_real_escape_string(sql, &userEsc[0], username.c_str(), username.size());
        userEsc.resize(userLen);

        std::string passEsc(password.size() * 2 + 1, '\0');
        unsigned long passLen = mysql_real_escape_string(sql, &passEsc[0], password.c_str(), password.size());
        passEsc.resize(passLen);

        std::string query = "SELECT username FROM user WHERE username='" + userEsc +
                            "' AND password='" + passEsc + "' LIMIT 1";
        bool exists = false;

        if (mysql_query(sql, query.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(sql);
            if (res) {
                exists = (mysql_num_rows(res) > 0);
                mysql_free_result(res);
            }
        } else if (mysql_errno(sql) == 1054) {
            // Compatible with legacy schema: table `user` only has `username`.
            std::string legacyQuery = "SELECT username FROM user WHERE username='" + userEsc + "' LIMIT 1";
            if (mysql_query(sql, legacyQuery.c_str()) == 0) {
                MYSQL_RES* res = mysql_store_result(sql);
                if (res) {
                    exists = (mysql_num_rows(res) > 0);
                    mysql_free_result(res);
                }
            } else {
                LOG_ERROR("SQL Query Error!");
            }
        } else {
            LOG_ERROR("SQL Query Error!");
        }
        FreeConn(sql);
        return exists;
#else
        (void)username;
        (void)password;
        return false;
#endif
    }

private:
    SqlConnPool() {}
    ~SqlConnPool() {
#if SQL_CONNPOOL_HAS_MYSQL
        while(!connQue_.empty()) {
            auto item = connQue_.front();
            connQue_.pop();
            mysql_close(item);
        }
        mysql_library_end();
#endif
    }

    std::queue<MYSQL*> connQue_;
    std::mutex mtx_;
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
