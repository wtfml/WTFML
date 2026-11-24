/**
 * @file ServerPortCheck.hpp
 * @brief 「这个端口现在有人占着吗」那一件小事（「启动前端口占用检查」）。
 *
 * 【为什么单独一个文件、为什么它不属于 `ServerCheck`】
 *
 * `ServerCheck.hpp` 是**纯值进、结论出**（喂几个事实，回一张体检单）—— 验证工程拿它当纯函数
 * 钉。而"端口占没占"这件事**必须真去占一次**才知道（Windows 上没有"查一下这个端口谁在用"
 * 这种不需要权限的接口；`netstat` 要解析一份会变格式的文本，看一眼还容易看成"别人在用"）。
 *
 * 所以它被单独摘出来：一个枚举 + 一个薄函数，谁需要谁调。体检那一层只收**结论**
 * （`ServerCheck::Facts::portState`），于是它自己照旧是纯的。
 *
 * 【它到底做了什么】
 *
 * 在 `0.0.0.0` 上`listen()` 一次，成了 ⇒ 没人占（立刻关掉）；没成 ⇒ 有人占（或者是防火墙/
 * 权限不许我们监听，那种情况一律报 `Unknown`，**不当成"占用"** —— 把"查不出来"说成"有人在
 * 用"会让用户去关一个根本没开的程序)。
 */

#pragma once

#include <QString>

namespace ServerPortCheck {

/// 那个端口现在的样子。**三档**（"查不出来"和"有人占着"是两件事，见文件头）。
enum class State {
    Unknown,  ///< 没查（端口不合法 / 没有权限监听 / 别的报错）
    Free,     ///< 现在没人占
    InUse,    ///< 现在有人占
};

/// 去占一次 @p port（立刻放开）。@p problemOut 非空时回"为什么没查成"。
State stateOf(int port, QString *problemOut = nullptr);

/// 那一档状态摆给用户看的那句话（唯一出处）。
inline QString stateName(State state) {
    switch (state) {
    case State::Free:
        return QStringLiteral("没被占用");
    case State::InUse:
        return QStringLiteral("被占用了");
    case State::Unknown:
        return QStringLiteral("没能查出来");
    }
    return QString();
}

}  // namespace ServerPortCheck
