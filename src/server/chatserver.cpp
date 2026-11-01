#include "chatserver.hpp"
#include "chatservice.hpp"
#include "net/frame_codec.hpp"
#include "json.hpp"
#include <muduo/base/Logging.h>
#include <functional>
#include<string>
using namespace std;
using namespace placeholders;
using json = nlohmann::json;

// 初始化聊天服务器对象
ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg)
    : _server(loop, listenAddr, nameArg), _loop(loop)
{
    // 注册连接回调
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));
    // 注册消息回调
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, _1, _2, _3));
    // 设置线程数量
    _server.setThreadNum(4);
}

// 启动服务
void ChatServer::start()
{
    _server.start();
}

// 上报连接相关信息的回调函数
void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    //客户端断开连接
    if(!conn->connected())
    {
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

// 收到客户端发来的数据时，muduo 会自动调用这个函数。
//
// 一次调用可能收到：
//   - 半条消息（客户端还没发完）
//   - 一条完整消息
//   - 多条完整消息（TCP 把几次发送合并了）
//
// 这个函数负责从 buffer 里一条一条切出完整消息，再按 msgid 交给对应的业务函数。
void ChatServer::onMessage(const TcpConnectionPtr &conn,
                           Buffer *buffer,
                           Timestamp time)
{
    // buffer：muduo 给这条连接维护的"收件箱"，里面是已经收到、但还没处理的字节。
    //         readableBytes()：收件箱里现在有多少字节可以读。
    //         peek()：看一眼收件箱最前面的数据（不删除）。
    //         retrieve(n)：处理完 n 个字节后，把它们从收件箱里删掉。

    // 只要还剩至少 4 字节，就说明"可能"能读出一个长度头，尝试切一条消息。
    while (buffer->readableBytes() >= frame::kHeaderSize)
    {
        // body：切出来的一条完整消息的正文（JSON 字符串），还没有解析成 json 对象。
        string body;

        // consumed：这条消息一共占多少字节（4 字节长度头 + JSON 正文）。
        //           只有 extract 返回 Ok 时才有意义。
        size_t consumed = 0;

        // status：尝试切一条消息的结果。
        frame::Status status = frame::extract(buffer->peek(),
                                              buffer->readableBytes(),
                                              body, consumed);

        if (status == frame::Status::NeedMore)
        {
            // 半条消息。不要删 buffer 里的数据，等下一次客户端发数据来再拼。
            break;
        }
        if (status != frame::Status::Ok)
        {
            // 长度头不合法，这条连接上的数据已经无法再对齐，只能断开当前连接。
            LOG_WARN << "protocol error from " << conn->peerAddress().toIpPort()
                     << ", status=" << static_cast<int>(status)
                     << ", readable=" << buffer->readableBytes();
            conn->shutdown();
            return;
        }

        // 这条消息已经完整取出来了，把它的字节从 buffer 里删掉，
        // 否则下一轮还会从同一条消息开始，造成重复处理。
        buffer->retrieve(consumed);

        try
        {
            // js：把 JSON 字符串解析成对象，后面靠 js["msgid"] 判断消息类型。
            json js = json::parse(body);
            if (!js.is_object() || !js.contains("msgid") || !js["msgid"].is_number_integer())
            {
                LOG_WARN << "message without valid msgid from "
                         << conn->peerAddress().toIpPort();
                conn->shutdown();
                return;
            }

            // msgHandler：根据 msgid 找到对应的业务处理函数，
            //             比如登录、注册、单聊、群聊。
            auto msgHandler = ChatService::instance()->getHandler(js["msgid"].get<int>());
            msgHandler(conn, js, time);
        }
        catch (const json::exception &e)
        {
            // JSON 内容坏了（比如不是合法 JSON）。
            // 只断开当前连接，服务进程和其他客户端不受影响。
            LOG_WARN << "invalid message from " << conn->peerAddress().toIpPort()
                     << ": " << e.what();
            conn->shutdown();
            return;
        }

        // 业务函数可能已经主动关闭了连接（比如出错）。
        // 如果已经断了，就没必要再处理 buffer 里剩下的消息。
        if (!conn->connected())
        {
            return;
        }
    }
}
