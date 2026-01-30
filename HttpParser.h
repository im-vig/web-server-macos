#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include "Buffer.h"

class HttpParser {
public:
    enum PARSE_STATE { REQUEST_LINE, HEADERS, BODY, FINISH };

    HttpParser() : state_(REQUEST_LINE) {}

    bool parse(Buffer& buff) {
        while (state_ != FINISH) {
            // 直接在内存中寻找 \r\n，性能极高
            const char* CRLF = std::search(buff.peek(), (const char*)buff.beginWrite(), kCRLF, kCRLF + 2);
            
            // 如果没找到 \r\n，且不是在解析 Body，说明数据没到齐
            if (CRLF == buff.beginWrite() && state_ != BODY) return true;

            switch (state_) {
                case REQUEST_LINE:
                    if (!parseRequestLine(buff.peek(), CRLF)) return false;
                    buff.retrieveUntil(CRLF + 2);
                    state_ = HEADERS;
                    break;
                case HEADERS:
                    if (CRLF == buff.peek()) { // 遇到空行 \r\n\r\n
                        buff.retrieve(2);
                        state_ = FINISH;
                    } else {
                        parseHeader(buff.peek(), CRLF);
                        buff.retrieveUntil(CRLF + 2);
                    }
                    break;
                case BODY:
                    // 暂不处理 Body，直接跳过或存储
                    state_ = FINISH;
                    break;
                default: break;
            }
        }
        return true;
    }

    std::string path() const { return path_; }
    bool isFinish() const { return state_ == FINISH; }
    bool isKeepAlive() const { return isKeepAlive_; }

private:
    bool parseRequestLine(const char* start, const char* end) {
        // 使用原生的字符串搜索，避免频繁创建 string
        const char* space1 = std::find(start, end, ' ');
        if (space1 == end) return false;
        method_ = std::string(start, space1);

        const char* space2 = std::find(space1 + 1, end, ' ');
        if (space2 == end) return false;
        path_ = std::string(space1 + 1, space2);
        
        return true;
    }

    void parseHeader(const char* start, const char* end) {
        const char* colon = std::find(start, end, ':');
        if (colon != end) {
            std::string key(start, colon);
            std::string value(colon + 2, end);
            if (key == "Connection") {
                isKeepAlive_ = (value == "keep-alive" || value == "Keep-Alive");
            }
        }
    }

   

    PARSE_STATE state_;
    std::string method_, path_;
    bool isKeepAlive_ = false;
    static const char kCRLF[];
};

const char HttpParser::kCRLF[] = "\r\n";

#endif