#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <unordered_map>
#include <string>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include "Log.h"

class HttpResponse {
public:
    HttpResponse() {
        mmFile_ = nullptr;
        mmFileStat_ = {0};
    }

    ~HttpResponse() { unmapFile(); }

    void init(const std::string& srcDir, const std::string& path, bool isKeepAlive = false, int code = -1) {
        if(mmFile_) unmapFile();
        srcDir_ = srcDir;
        path_ = path;
        isKeepAlive_ = isKeepAlive;
        code_ = code;
        mmFile_ = nullptr;
        mmFileStat_ = {0};
    }

    // 判断请求的文件是否存在、是否可读
    void makeResponse(std::string& header, std::string& body) {
        std::string safePath;
        if (!sanitizePath_(path_, safePath)) {
            code_ = 403;
            path_ = "/403.html";
        } else {
            path_ = safePath;
        }

        if (code_ == -1) {
            if(stat((srcDir_ + path_).c_str(), &mmFileStat_) < 0 || S_ISDIR(mmFileStat_.st_mode)) {
                code_ = 404;
            } else if(!(mmFileStat_.st_mode & S_IROTH)) {
                code_ = 403;
            } else {
                code_ = 200;
            }
        }
        errorHtml_();
        addStateLine_(header);
        addHeader_(header);
        // 如果是 200 OK，则进行内存映射
        if(code_ == 200) {
            int srcFd = open((srcDir_ + path_).c_str(), O_RDONLY);
            if(srcFd < 0) {
                errorHtml_(); // 再次检查
            } else {
                // 使用 mmap 将文件映射到内存，提升发送速度
                mmFile_ = (char*)mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
                close(srcFd);
            }
        }
    }

    char* file() { return mmFile_; }
    size_t fileSize() { return mmFileStat_.st_size; }
    void unmapFile() {
        if(mmFile_) {
            munmap(mmFile_, mmFileStat_.st_size);
            mmFile_ = nullptr;
        }
    }

private:
    bool sanitizePath_(const std::string& raw, std::string& out) {
        if (raw.empty() || raw[0] != '/' || raw.find('\0') != std::string::npos) return false;

        std::string path = raw;
        size_t qPos = path.find('?');
        if (qPos != std::string::npos) path = path.substr(0, qPos);

        std::vector<std::string> segs;
        size_t i = 1;
        while (i <= path.size()) {
            size_t j = path.find('/', i);
            if (j == std::string::npos) j = path.size();
            std::string token = path.substr(i, j - i);
            if (token == "..") return false;
            if (!token.empty() && token != ".") segs.push_back(token);
            i = j + 1;
        }

        out = "/";
        for (size_t idx = 0; idx < segs.size(); ++idx) {
            out += segs[idx];
            if (idx + 1 < segs.size()) out += "/";
        }
        if (out == "/") out = "/index.html";
        return true;
    }

    void addStateLine_(std::string& header) {
        header += "HTTP/1.1 " + std::to_string(code_) + " " + SUFFIX.at(code_) + "\r\n";
    }

    void addHeader_(std::string& header) {
        header += "Connection: " + (isKeepAlive_ ? std::string("keep-alive") : std::string("close")) + "\r\n";
        header += "Content-type: " + getFileType_() + "\r\n";
        header += "Content-length: " + std::to_string(mmFileStat_.st_size) + "\r\n\r\n";
    }

    void errorHtml_() {
        if(code_ == 404) path_ = "/404.html";
        else if(code_ == 403) path_ = "/403.html";
        if(code_ != 200) stat((srcDir_ + path_).c_str(), &mmFileStat_);
    }

    std::string getFileType_() {
        size_t idx = path_.find_last_of('.');
        if(idx == std::string::npos) return "text/plain";
        std::string suffix = path_.substr(idx);
        if(MIME_TYPE.count(suffix)) return MIME_TYPE.at(suffix);
        return "text/plain";
    }

    int code_;
    bool isKeepAlive_;
    std::string path_;
    std::string srcDir_;
    char* mmFile_;
    struct stat mmFileStat_;

    static const std::unordered_map<std::string, std::string> MIME_TYPE;
    static const std::unordered_map<int, std::string> SUFFIX;
};

const std::unordered_map<std::string, std::string> HttpResponse::MIME_TYPE = {
    {".html", "text/html"}, {".xml", "text/xml"}, {".txt", "text/plain"},
    {".jpg", "image/jpeg"}, {".png", "image/png"}, {".gif", "image/gif"},
    {".css", "text/css"},   {".js", "text/javascript"}, {".zip", "application/zip"}
};

const std::unordered_map<int, std::string> HttpResponse::SUFFIX = {
    {200, "OK"}, {400, "Bad Request"}, {403, "Forbidden"}, {404, "Not Found"}
};

#endif
