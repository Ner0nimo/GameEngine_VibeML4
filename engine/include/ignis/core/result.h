#pragma once

#include <string>
#include <utility>

namespace ignis {

// Result<T>: a success value and an error message in one object, so a
// function signature communicates both outcomes on its own — callers never
// juggle a bool plus a separate out-parameter. ok() == error().empty().
// Use Result<void> for operations that only need success + message.
template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value), {}); }
    static Result failure(std::string error) { return Result(T{}, std::move(error)); }

    bool ok() const noexcept { return error_.empty(); }
    explicit operator bool() const noexcept { return ok(); }
    const std::string& error() const noexcept { return error_; }

    T& value() & noexcept { return value_; }
    const T& value() const& noexcept { return value_; }
    T&& value() && noexcept { return std::move(value_); }

private:
    Result(T v, std::string e) : value_(std::move(v)), error_(std::move(e)) {}

    T value_{};
    std::string error_;
};

template <>
class Result<void> {
public:
    Result() = default; // success
    static Result success() { return Result(); }
    static Result failure(std::string error) { return Result(std::move(error)); }

    bool ok() const noexcept { return error_.empty(); }
    explicit operator bool() const noexcept { return ok(); }
    const std::string& error() const noexcept { return error_; }

private:
    explicit Result(std::string e) : error_(std::move(e)) {}

    std::string error_;
};

} // namespace ignis
