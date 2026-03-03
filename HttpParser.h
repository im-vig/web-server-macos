#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cctype>
#include "Buffer.h"

class HttpParser {
public:
    enum PARSE_STATE { REQUEST_LINE, HEADERS, BODY, FINISH };
    enum PARSE_RESULT { PARSE_ERROR, PARSE_AGAIN, PARSE_FINISH };

    HttpParser() { reset(); }

    PARSE_RESULT parse(Buffer& buff) {
        while (state_ != FINISH) {
            if (state_ == BODY) {
                if (buff.readableBytes() < contentLength_) {
                    return PARSE_AGAIN;
                }
                body_.assign(buff.peek(), buff.peek() + contentLength_);
                buff.retrieve(contentLength_);
                state_ = FINISH;
                break;
            }

            // 直接在内存中寻找 \r\n，性能极高
            const char* CRLF = std::search(buff.peek(), (const char*)buff.beginWrite(), kCRLF, kCRLF + 2);
            
            // 如果没找到 \r\n，且不是在解析 Body，说明数据没到齐
            if (CRLF == buff.beginWrite()) return PARSE_AGAIN;

            switch (state_) {
                case REQUEST_LINE:
                    if (!parseRequestLine(buff.peek(), CRLF)) return PARSE_ERROR;
                    buff.retrieveUntil(CRLF + 2);
                    state_ = HEADERS;
                    break;
                case HEADERS:
                    if (CRLF == buff.peek()) { // 遇到空行 \r\n\r\n
                        buff.retrieve(2);
                        state_ = (contentLength_ > 0) ? BODY : FINISH;
                    } else {
                        parseHeader(buff.peek(), CRLF);
                        buff.retrieveUntil(CRLF + 2);
                    }
                    break;
                case BODY:
                    break;
                default: break;
            }
        }
        return (state_ == FINISH) ? PARSE_FINISH : PARSE_AGAIN;
    }

    void reset() {
        state_ = REQUEST_LINE;
        method_.clear();
        path_.clear();
        body_.clear();
        isKeepAlive_ = false;
        contentLength_ = 0;
    }

    std::string method() const { return method_; }
    std::string path() const { return path_; }
    std::string body() const { return body_; }
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
            const char* valueStart = colon + 1;
            while (valueStart < end && *valueStart == ' ') valueStart++;
            std::string value(valueStart, end);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });

            if (key == "content-length") {
                try {
                    contentLength_ = static_cast<size_t>(std::stoul(value));
                } catch (...) {
                    contentLength_ = 0;
                }
            }
            if (key == "connection") {
                isKeepAlive_ = (value == "keep-alive");
            }
        }
    }

   

    PARSE_STATE state_;
    std::string method_, path_, body_;
    bool isKeepAlive_ = false;
    size_t contentLength_ = 0;
    static const char kCRLF[];
};

const char HttpParser::kCRLF[] = "\r\n";

#endif
