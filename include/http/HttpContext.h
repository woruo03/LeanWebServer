/**
 * @file HttpContext.h
 * @brief 零拷贝流式 HTTP 请求解析上下文
 * @author woruo03
 * @date 2026-11-01
 * @version 1.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace lean_web_server::net {
class Buffer;
}

namespace lean_web_server::http {

/**
 * @class HttpContext
 * @brief 状态机流式 HTTP 请求解析上下文
 *
 * 核心机制：
 * - 纯零拷贝设计：不复制任何字符串，内部基于 Span {offset, length} 记录在底层 Buffer 中的偏移；
 * - 状态机逐阶段驱动：kExpectRequestLine -> kExpectHeaders -> kExpectBody -> kDone；
 * - 请求头大小写不敏感匹配：支持 Connection: keep-alive 等标准 HTTP 协议解析。
 */
class HttpContext {
public:
    /// HTTP 请求解析状态机状态
    enum class HttpRequestParseState : uint8_t {
        kExpectRequestLine,  ///< 正在等待/解析请求行
        kExpectHeaders,      ///< 正在解析头部字段
        kExpectBody,         ///< 正在根据 Content-Length 读取请求体
        kDone,               ///< 当前完整请求已解析完成
    };

    [[nodiscard]] HttpRequestParseState parse_state() const { return parse_state_; }

    /// 获取 HTTP 方法字符串视图（如 "GET", "POST"）
    [[nodiscard]] std::string_view method(const char* data) const { return method_.as_view(data); }

    /// 获取请求路径字符串视图（如 "/" 或 "/index.html"）
    [[nodiscard]] std::string_view path(const char* data) const { return path_.as_view(data); }

    /// 获取 HTTP 协议版本（如 "HTTP/1.1"）
    [[nodiscard]] std::string_view version(const char* data) const {
        return version_.as_view(data);
    }

    /// 获取指定名称的 HTTP 头部字段值（大小写不敏感匹配）
    [[nodiscard]] std::string_view get_header(const char* data, std::string_view key) const {
        for (const auto& [k, v] : headers_) {
            if (case_insensitive_equal(k.as_view(data), key)) {
                return v.as_view(data);
            }
        }
        return {};
    }

    /// 判断当前请求是否要求 Keep-Alive 长连接保活
    [[nodiscard]] bool keep_alive(const char* data) const {
        std::string_view conn = get_header(data, "Connection");
        if (case_insensitive_equal(conn, "keep-alive")) {
            return true;
        }
        if (case_insensitive_equal(conn, "close")) {
            return false;
        }
        return version(data) == "HTTP/1.1";
    }

    /// 获取当前请求已校验处理的字节总数
    [[nodiscard]] size_t checked_bytes() const { return checked_bytes_; }

    /// 重置状态机，准备解析同连接上的下一个管线化/复用请求
    void reset() {
        parse_state_ = HttpRequestParseState::kExpectRequestLine;
        checked_bytes_ = 0;
        method_ = {};
        path_ = {};
        version_ = {};
        headers_.clear();
        body_ = {};
        body_length_ = 0;
    }

    /// 解析缓冲区中的 HTTP 报文数据
    bool parse_request(net::Buffer& buffer);

private:
    struct Span {
        size_t offset{0};
        size_t length{0};

        [[nodiscard]] std::string_view as_view(const char* base) const {
            return {base + offset, length};
        }
    };

    [[nodiscard]] const char* find_crlf(const net::Buffer& buffer) const;
    static bool case_insensitive_equal(std::string_view a, std::string_view b) noexcept;

    bool process_request_line(const char* begin, const char* end);
    bool process_headers(const char* begin, const char* end);

    bool parse_request_line_state(net::Buffer& buffer, bool& has_more);
    bool parse_headers_state(net::Buffer& buffer, bool& has_more);
    void parse_body_state(net::Buffer& buffer, bool& has_more);

    HttpRequestParseState parse_state_{HttpRequestParseState::kExpectRequestLine};
    size_t checked_bytes_{0};

    Span method_;
    Span path_;
    Span version_;
    std::vector<std::pair<Span, Span>> headers_;
    Span body_;
    size_t body_length_{0};
};

}  // namespace lean_web_server::http
