#ifndef BUFFER_H
#define BUFFER_H

#include <vector>
#include <string>
#include <algorithm>
#include <sys/uio.h>
#include <unistd.h>

class Buffer {
public:
    Buffer(int initSize = 1024) : buffer_(initSize), readPos_(0), writePos_(0) {}

    size_t readableBytes() const { return writePos_ - readPos_; }
    size_t writableBytes() const { return buffer_.size() - writePos_; }

    // 返回当前可读数据的起始地址
    const char* peek() const { return &buffer_[readPos_]; }
    // 返回当前可写数据的起始地址
    char* beginWrite() { return &buffer_[writePos_]; }

    void retrieve(size_t len) {
        if (len < readableBytes()) readPos_ += len;
        else readPos_ = writePos_ = 0;
    }

    void retrieveUntil(const char* end) { retrieve(end - peek()); }

    ssize_t readFd(int fd, int* Errno) {
        char extrabuf[65535];
        struct iovec iov[2];
        const size_t writable = writableBytes();
        iov[0].iov_base = beginWrite();
        iov[0].iov_len = writable;
        iov[1].iov_base = extrabuf;
        iov[1].iov_len = sizeof(extrabuf);

        const ssize_t len = readv(fd, iov, 2);
        if (len < 0) *Errno = errno;
        else if (static_cast<size_t>(len) <= writable) writePos_ += len;
        else {
            writePos_ = buffer_.size();
            append(extrabuf, len - writable);
        }
        return len;
    }

    void append(const char* str, size_t len) {
        if (writableBytes() < len) makeSpace(len);
        std::copy(str, str + len, beginWrite());
        writePos_ += len;
    }

private:
    void makeSpace(size_t len) {
        if (writableBytes() + readPos_ < len) buffer_.resize(writePos_ + len + 1);
        else {
            size_t readable = readableBytes();
            std::copy(buffer_.begin() + readPos_, buffer_.begin() + writePos_, buffer_.begin());
            readPos_ = 0; writePos_ = readable;
        }
    }
    std::vector<char> buffer_;
    size_t readPos_, writePos_;
};
#endif