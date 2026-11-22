#include "http/HttpContext.h"

#include <cctype>
#include <charconv>

#include "net/Buffer.h"

namespace lean_web_server::http {

bool HttpContext::parse_request_line_state(net::Buffer& buffer, bool& has_more) {
    const char* crlf = find_crlf(buffer);
    if (crlf == nullptr) {
        has_more = false;
        return true;
    }

    const char* begin = buffer.read_ptr() + checked_bytes_;
    const char* end = crlf;
    if (!process_request_line(begin, end)) {
        return false;
    }
    parse_state_ = HttpRequestParseState::kExpectHeaders;
    checked_bytes_ = crlf - buffer.read_ptr() + 2;
    return true;
}

bool HttpContext::parse_headers_state(net::Buffer& buffer, bool& has_more) {
    const char* crlf = find_crlf(buffer);
    if (crlf == nullptr) {
        has_more = false;
        return true;
    }

    const char* begin = buffer.read_ptr() + checked_bytes_;
    const char* end = crlf;
    if (begin == end) {
        if (body_length_ > 0) {
            parse_state_ = HttpRequestParseState::kExpectBody;
        } else {
            parse_state_ = HttpRequestParseState::kDone;
        }
        checked_bytes_ = crlf - buffer.read_ptr() + 2;
        return true;
    }
    if (!process_headers(begin, end)) {
        return false;
    }
    checked_bytes_ = crlf - buffer.read_ptr() + 2;
    return true;
}

void HttpContext::parse_body_state(net::Buffer& buffer, bool& has_more) {
    if (SIZE_MAX - checked_bytes_ < body_length_ ||
        buffer.read_available() < checked_bytes_ + body_length_) {
        has_more = false;
        return;
    }

    body_ = {checked_bytes_, body_length_};
    parse_state_ = HttpRequestParseState::kDone;
    checked_bytes_ += body_length_;
}

bool HttpContext::parse_request(net::Buffer& buffer) {
    bool has_more = true;
    while (has_more) {
        switch (parse_state_) {
            case HttpRequestParseState::kExpectRequestLine:
                if (!parse_request_line_state(buffer, has_more)) {
                    return false;
                }
                break;
            case HttpRequestParseState::kExpectHeaders:
                if (!parse_headers_state(buffer, has_more)) {
                    return false;
                }
                break;
            case HttpRequestParseState::kExpectBody:
                parse_body_state(buffer, has_more);
                break;
            case HttpRequestParseState::kDone:
                has_more = false;
                break;
        }
    }
    return true;
}

// 解析请求行
bool HttpContext::process_request_line(const char* begin, const char* end) {
    std::string_view sv(begin, end - begin);
    auto pos1 = sv.find(' ');
    if (pos1 == std::string_view::npos) {  // 没有找到第一个空格，解析失败
        return false;
    }
    auto pos2 = sv.find(' ', pos1 + 1);
    if (pos2 == std::string_view::npos) {  // 没有找到第二个空格，解析失败
        return false;
    }

    if (pos1 == 0 || pos2 == pos1 + 1 ||
        pos2 == sv.size() - 1) {  // 方法、路径或版本号为空，解析失败
        return false;
    }
    std::string_view ver = sv.substr(pos2 + 1);
    if (ver.rfind("HTTP/", 0) != 0) {  // 版本号必须以 HTTP/ 开头
        return false;
    }
    method_ = {0, pos1};
    path_ = {pos1 + 1, pos2 - pos1 - 1};
    version_ = {pos2 + 1, sv.size() - pos2 - 1};
    return true;
}

// 解析请求头
bool HttpContext::process_headers(const char* begin, const char* end) {
    std::string_view sv(begin, end - begin);
    auto pos = sv.find(':');
    if (pos == std::string_view::npos) {  // 没有找到冒号，解析失败
        return false;
    }

    size_t key_begin = 0, key_end = pos, value_begin = pos + 1, value_end = sv.size();
    if (key_end == key_begin) {  // key 为空，解析失败
        return false;
    }

    while (value_begin < value_end && (sv[value_begin] == ' ' || sv[value_begin] == '\t')) {
        value_begin++;
    }
    while (value_end > value_begin && (sv[value_end - 1] == ' ' || sv[value_end - 1] == '\t')) {
        value_end--;
    }

    std::string_view key_sv = sv.substr(key_begin, key_end - key_begin);
    std::string_view value_sv = sv.substr(value_begin, value_end - value_begin);
    if (case_insensitive_equal(key_sv, "Content-Length")) {
        if (body_length_ > 0) {  // 已经解析过 Content-Length，重复解析
            return false;
        }

        auto result =
            std::from_chars(value_sv.data(), value_sv.data() + value_sv.size(), body_length_);
        if (result.ec != std::errc() ||
            result.ptr != value_sv.data() + value_sv.size()) {  // 解析 Content-Length 失败
            return false;
        }
    }

    // 将解析后的键值对存储到 headers_ 中
    headers_.push_back({{key_begin + checked_bytes_, key_end - key_begin},
                        {value_begin + checked_bytes_, value_end - value_begin}});

    return true;
}

const char* HttpContext::find_crlf(const net::Buffer& buffer) const {
    std::string_view sv(buffer.read_ptr() + checked_bytes_,
                        buffer.read_available() - checked_bytes_);
    auto result = sv.find("\r\n");
    return result != std::string_view::npos ? sv.data() + result : nullptr;
}

bool HttpContext::case_insensitive_equal(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
               return std::tolower(static_cast<unsigned char>(c1)) ==
                      std::tolower(static_cast<unsigned char>(c2));
           });
}

}  // namespace lean_web_server::http
