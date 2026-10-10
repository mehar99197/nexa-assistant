#pragma once

#include <QString>

#include <optional>
#include <utility>

namespace nexa {

enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    NotSupported,
    Io,
    Parse,
    Network,
    ConnectionRefused,
    Timeout,
    Http,
    Unauthorized,
    PermissionDenied,
    Cancelled,
};

struct Error {
    ErrorCode code{ErrorCode::Unknown};
    QString message;
};

// Value-or-error return type. Nexa does not throw: every fallible call returns a Result<T>,
// and asynchronous APIs deliver one through their callback.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    bool isOk() const { return value_.has_value(); }
    explicit operator bool() const { return isOk(); }

    T &value() & { return *value_; }
    const T &value() const & { return *value_; }
    T &&value() && { return std::move(*value_); }

    const Error &error() const { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    bool isOk() const { return ok_; }
    explicit operator bool() const { return ok_; }

    const Error &error() const { return error_; }

private:
    Error error_;
    bool ok_{true};
};

} // namespace nexa
