/**
 * @file NonCopyable.h
 * @brief 不可拷贝基类定义，用于显式禁止派生类的拷贝构造与赋值语义
 * @author woruo03
 * @date 2026-11-13
 * @version 1.0
 */

#pragma once

namespace lean_web_server::base {

/**
 * @class NonCopyable
 * @brief 标识类不可被拷贝的空基类
 *
 * 派生类私有或公有继承此类后，其默认的拷贝构造函数与拷贝赋值操作符将被编译器隐式删除。
 * 析构函数与默认构造函数为 protected，防止外部通过 NonCopyable* 进行多态删除。
 */
class NonCopyable {
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};

}  // namespace lean_web_server::base
