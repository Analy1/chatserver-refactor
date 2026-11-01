#ifndef FRAME_CODEC_HPP
#define FRAME_CODEC_HPP


#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "json.hpp"

namespace frame {

constexpr size_t kHeaderSize = 4; // 长度头 4 字节
// 单帧上限 256KB：聊天消息远小于此，登录响应在重度用户下可能到几十 KB。
constexpr uint32_t kMaxBodySize = 256 * 1024;

enum class Status {
    Ok,         // 成功解出一帧
    NeedMore,   // 数据不足，等待下一次读
    TooLarge,   // 长度头超过上限，协议被破坏
    Malformed,  // 零长度等非法帧
};

// 从 data[0, len) 尝试解出一帧。
// 返回 Ok 时：body 被赋值，consumed = 4 + bodyLen。
// 返回其它状态时：body 不变，consumed = 0，调用方保留数据等待或断开。
inline Status extract(const char *data, size_t len, std::string &body,
                      size_t &consumed, uint32_t maxBody = kMaxBodySize)
{
    consumed = 0;
    if (len < kHeaderSize)
    {
        return Status::NeedMore;
    }

    uint32_t netLen = 0;
    std::memcpy(&netLen, data, kHeaderSize);
    const uint32_t bodyLen = ntohl(netLen); 

    if (bodyLen == 0)
    {
        return Status::Malformed;
    }
    if (bodyLen > maxBody)
    {
        return Status::TooLarge;
    }
    if (len < kHeaderSize + static_cast<size_t>(bodyLen))
    {
        return Status::NeedMore;
    }

    body.assign(data + kHeaderSize, bodyLen);
    consumed = kHeaderSize + static_cast<size_t>(bodyLen);
    return Status::Ok;
}

// 将 body 编码为一帧，返回完整的帧数据。
inline std::string encode(const std::string &body)
{
    const uint32_t netLen = htonl(static_cast<uint32_t>(body.size()));
    std::string out;
    out.reserve(kHeaderSize + body.size());
    out.append(reinterpret_cast<const char *>(&netLen), kHeaderSize);
    out.append(body);
    return out;
}

inline std::string encode(const nlohmann::json &js)
{
    return encode(js.dump());
}

// 在（阻塞）socket 上发完整个缓冲区，处理短写、EINTR 和 EAGAIN。
inline bool sendAll(int fd, const std::string &data, int timeoutMs = 3000)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        // MSG_NOSIGNAL: 防止 SIGPIPE 信号导致进程退出
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) // 非阻塞 socket 写满了
        {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int rc = 0;
            do
            {
                rc = ::poll(&pfd, 1, timeoutMs);
            } while (rc < 0 && errno == EINTR);
            if (rc > 0)
            {
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

inline bool sendJson(int fd, const nlohmann::json &js, int timeoutMs = 3000)
{
    return sendAll(fd, encode(js), timeoutMs);
}

} // namespace frame

#endif // FRAME_CODEC_HPP
