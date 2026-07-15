#pragma once

#include <string>
#include <variant>

namespace ctee {

template <typename T>
class Result {
public:
    static Result success(T value) {
        Result r;
        r.data_ = std::move(value);
        r.success_ = true;
        return r;
    }

    static Result error(std::string message) {
        Result r;
        r.data_ = std::move(message);
        r.success_ = false;
        return r;
    }

    [[nodiscard]] bool is_success() const noexcept { return success_; }

    [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
    [[nodiscard]] T value() && { return std::get<T>(std::move(data_)); }

    [[nodiscard]] const std::string& error() const& { return std::get<std::string>(data_); }

private:
    Result() = default;
    bool success_ = false;
    std::variant<T, std::string> data_;
};

}  // namespace ctee
