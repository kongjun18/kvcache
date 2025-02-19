#include "status.h"

namespace KVCache {

Status::Status(Code code, const std::string& msg) noexcept : code_(code), msg_(msg) {}

Status::~Status() noexcept {}

bool Status::ok() const {
    return code_ == kOK;
}   

Code Status::code() const {
    return code_;
}

bool Status::is_not_found() const {
    return code_ == kNotFound;
}

std::string Status::msg() const {
    return msg_;
}

Status Status::OK() {
    return Status(kOK, "");
}

Status Status::NotFound(const std::string& msg= std::string()) {
    return Status(kNotFound, msg);
}

Status Status::Corruption(const std::string& msg= std::string()) {
    return Status(kCorruption, msg);
}

Status Status::ObjectTooLarge(const std::string& msg= std::string()) {
    return Status(kObjectTooLarge, msg);
}

}

