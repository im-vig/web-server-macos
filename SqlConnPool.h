#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <string>
#include <queue>
#include <mutex>
#include <vector>
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

#if SQL_CONNPOOL_HAS_MYSQL
#include <openssl/evp.h>
#include <openssl/rand.h>
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
            ensureAuthSchema_(sql);
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
        ensureAuthSchema_(sql);

        std::string userEsc = escapeString_(sql, username);
        bool hasLegacy = columnExists_(sql, "password");
        std::string query = hasLegacy
            ? "SELECT password_hash, password_salt, password FROM user WHERE username='" + userEsc + "' LIMIT 1"
            : "SELECT password_hash, password_salt, '' FROM user WHERE username='" + userEsc + "' LIMIT 1";

        bool ok = false;
        if (mysql_query(sql, query.c_str()) != 0) {
            LOG_ERROR("SQL Query Error!");
            FreeConn(sql);
            return false;
        }
        MYSQL_RES* res = mysql_store_result(sql);
        if (!res) {
            FreeConn(sql);
            return false;
        }
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) {
            mysql_free_result(res);
            FreeConn(sql);
            return false;
        }
        std::string hash = row[0] ? row[0] : "";
        std::string salt = row[1] ? row[1] : "";
        std::string legacyPass = row[2] ? row[2] : "";
        mysql_free_result(res);

        if (!hash.empty() && !salt.empty()) {
            std::string calcHash;
            if (hashPassword_(password, salt, calcHash)) {
                ok = (calcHash == hash);
            }
        } else if (hasLegacy && !legacyPass.empty() && legacyPass == password) {
            // One-time upgrade for old plaintext rows.
            std::string saltHex;
            std::string hashHex;
            if (generateSalt_(saltHex) && hashPassword_(password, saltHex, hashHex)) {
                std::string upgrade =
                    "UPDATE user SET password_hash='" + hashHex + "', password_salt='" + saltHex +
                    "', password='' WHERE username='" + userEsc + "' LIMIT 1";
                mysql_query(sql, upgrade.c_str());
                ok = true;
            }
        }

        FreeConn(sql);
        return ok;
#else
        (void)username;
        (void)password;
        return false;
#endif
    }

    bool UserExists(const std::string& username) {
#if SQL_CONNPOOL_HAS_MYSQL
        MYSQL* sql = GetConn();
        if (!sql) return false;

        std::string userEsc(username.size() * 2 + 1, '\0');
        unsigned long userLen = mysql_real_escape_string(sql, &userEsc[0], username.c_str(), username.size());
        userEsc.resize(userLen);

        std::string query = "SELECT username FROM user WHERE username='" + userEsc + "' LIMIT 1";
        bool exists = false;
        if (mysql_query(sql, query.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(sql);
            if (res) {
                exists = (mysql_num_rows(res) > 0);
                mysql_free_result(res);
            }
        } else {
            LOG_ERROR("SQL Query Error!");
        }
        FreeConn(sql);
        return exists;
#else
        (void)username;
        return false;
#endif
    }

    bool CreateUser(const std::string& username, const std::string& password) {
#if SQL_CONNPOOL_HAS_MYSQL
        MYSQL* sql = GetConn();
        if (!sql) return false;
        ensureAuthSchema_(sql);

        std::string userEsc = escapeString_(sql, username);
        std::string saltHex;
        std::string hashHex;
        if (!generateSalt_(saltHex) || !hashPassword_(password, saltHex, hashHex)) {
            FreeConn(sql);
            return false;
        }

        bool hasLegacy = columnExists_(sql, "password");
        std::string query = hasLegacy
            ? "INSERT INTO user(username, password_hash, password_salt, password) VALUES('" +
                  userEsc + "', '" + hashHex + "', '" + saltHex + "', '')"
            : "INSERT INTO user(username, password_hash, password_salt) VALUES('" +
                  userEsc + "', '" + hashHex + "', '" + saltHex + "')";
        bool ok = false;
        if (mysql_query(sql, query.c_str()) == 0) {
            ok = true;
        } else if (mysql_errno(sql) == 1062) {
            ok = false;
        } else {
            LOG_ERROR("SQL Insert Error!");
        }

        FreeConn(sql);
        return ok;
#else
        (void)username;
        (void)password;
        return false;
#endif
    }

private:
#if SQL_CONNPOOL_HAS_MYSQL
    std::string escapeString_(MYSQL* sql, const std::string& in) {
        std::string out(in.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(sql, &out[0], in.c_str(), in.size());
        out.resize(len);
        return out;
    }

    std::string bytesToHex_(const unsigned char* data, size_t len) {
        static const char HEX[] = "0123456789abcdef";
        std::string out;
        out.resize(len * 2);
        for (size_t i = 0; i < len; ++i) {
            out[2 * i] = HEX[(data[i] >> 4) & 0x0F];
            out[2 * i + 1] = HEX[data[i] & 0x0F];
        }
        return out;
    }

    bool hexToBytes_(const std::string& hex, std::vector<unsigned char>& out) {
        auto val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        if (hex.size() % 2 != 0) return false;
        out.clear();
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            int hi = val(hex[i]);
            int lo = val(hex[i + 1]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<unsigned char>((hi << 4) | lo));
        }
        return true;
    }

    bool generateSalt_(std::string& saltHex) {
        unsigned char salt[16];
        if (RAND_bytes(salt, sizeof(salt)) != 1) return false;
        saltHex = bytesToHex_(salt, sizeof(salt));
        return true;
    }

    bool hashPassword_(const std::string& password, const std::string& saltHex, std::string& hashHex) {
        std::vector<unsigned char> salt;
        if (!hexToBytes_(saltHex, salt)) return false;
        unsigned char out[32];
        const int iterations = 120000;
        if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                              salt.data(), static_cast<int>(salt.size()),
                              iterations, EVP_sha256(), sizeof(out), out) != 1) {
            return false;
        }
        hashHex = bytesToHex_(out, sizeof(out));
        return true;
    }

    bool columnExists_(MYSQL* sql, const std::string& col) {
        std::string query =
            "SELECT 1 FROM information_schema.columns WHERE table_schema=DATABASE() "
            "AND table_name='user' AND column_name='" + escapeString_(sql, col) + "' LIMIT 1";
        if (mysql_query(sql, query.c_str()) != 0) return false;
        MYSQL_RES* res = mysql_store_result(sql);
        if (!res) return false;
        bool exists = mysql_num_rows(res) > 0;
        mysql_free_result(res);
        return exists;
    }

    void ensureAuthSchema_(MYSQL* sql) {
        if (schemaReady_) return;

        bool hasHash = columnExists_(sql, "password_hash");
        bool hasSalt = columnExists_(sql, "password_salt");
        bool hasLegacy = columnExists_(sql, "password");

        if (!hasHash) {
            mysql_query(sql, "ALTER TABLE user ADD COLUMN password_hash VARCHAR(128) NOT NULL DEFAULT ''");
        }
        if (!hasSalt) {
            mysql_query(sql, "ALTER TABLE user ADD COLUMN password_salt VARCHAR(64) NOT NULL DEFAULT ''");
        }

        hasHash = columnExists_(sql, "password_hash");
        hasSalt = columnExists_(sql, "password_salt");
        hasLegacy = columnExists_(sql, "password");

        if (hasHash && hasSalt && hasLegacy) {
            if (mysql_query(sql,
                "SELECT id, password FROM user WHERE password IS NOT NULL AND password != '' "
                "AND (password_hash IS NULL OR password_hash='')") == 0) {
                MYSQL_RES* res = mysql_store_result(sql);
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res)) != nullptr) {
                        std::string id = row[0] ? row[0] : "";
                        std::string plain = row[1] ? row[1] : "";
                        if (id.empty() || plain.empty()) continue;
                        std::string saltHex;
                        std::string hashHex;
                        if (!generateSalt_(saltHex) || !hashPassword_(plain, saltHex, hashHex)) continue;
                        std::string update =
                            "UPDATE user SET password_hash='" + hashHex + "', password_salt='" + saltHex +
                            "', password='' WHERE id=" + id;
                        mysql_query(sql, update.c_str());
                    }
                    mysql_free_result(res);
                }
            }
        }

        schemaReady_ = true;
    }
#endif

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
    bool schemaReady_ = false;
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
