#ifndef STATUS_H
#define STATUS_H

#include <string>
namespace KVCache {

enum Code {
    kOK,
    kNotFound,
    kCorruption,
    kObjectTooLarge,
    kBufferTooSmall,
    kShutdown,
};

class Status {
public:
    Status() = default;
    Status(Code code, const std::string& msg) noexcept;
    ~Status()= default;

    bool ok() const;
    bool not_found() const;
    bool object_too_large() const;
    bool shutdown() const;
    bool buffer_too_small() const;
    Code code() const;
    std::string msg() const;

    static Status OK();
    static Status NotFound(const std::string& msg = std::string());
    static Status Corruption(const std::string& msg = std::string());
    static Status ObjectTooLarge(const std::string& msg = std::string());
    static Status Shutdown(const std::string& msg = std::string());
    static Status BufferTooSmall(const std::string& msg = std::string());
private:
    Code code_;
    std::string msg_;
};


}

#endif
