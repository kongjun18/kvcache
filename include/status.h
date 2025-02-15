#ifndef STATUS_H
#define STATUS_H

#include <string>
namespace KVCache {

enum Code {
    kOK,
    kNotFound,
    kCorruption,
};
class Status {
public:
    Status(Code code, const std::string& msg) noexcept;
    ~Status() noexcept;

    bool ok() const;
    Code code() const;
    std::string msg() const;

    static Status OK();
    static Status NotFound(const std::string& msg = std::string());
    static Status Corruption(const std::string& msg = std::string());
private:
    Code code_;
    std::string msg_;
};


}

#endif
