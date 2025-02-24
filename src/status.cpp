#include "status.h"

namespace KVCache {

Status::Status(Code code, const std::string& msg) noexcept : code_(code), msg_(msg) {}


bool Status::ok() const {
    return code_ == kOK;
}   

bool Status::shutdown() const {
    return code_ == kShutdown;
}

Code Status::code() const {
    return code_;
}

bool Status::not_found() const {
    return code_ == kNotFound;
}

bool Status::object_too_large() const {
    return code_ == kObjectTooLarge;
}

bool Status::buffer_too_small() const {
    return code_ == kBufferTooSmall;
}

std::string Status::msg() const {
    return msg_;
}

Status Status::Shutdown(const std::string& msg) {
    return Status(kShutdown, msg);
}

Status Status::OK() {
    return Status(kOK, "");
}

Status Status::NotFound(const std::string& msg) {
    return Status(kNotFound, msg);
}

Status Status::Corruption(const std::string& msg) {
    return Status(kCorruption, msg);
}

Status Status::ObjectTooLarge(const std::string& msg) {
    return Status(kObjectTooLarge, msg);
}

Status Status::BufferTooSmall(const std::string& msg) {
    return Status(kBufferTooSmall, msg);
}

}

