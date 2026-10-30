#include<muduo/net/TcpServer.h>
#include<muduo/net/EventLoop.h>
#include<iostream>
#include<functional>
#include<string>

using namespace std;
using namespace muduo;
using namespace muduo::net;
using namespace placeholders;

//基于网络muduo网络库开发服务器程序
/*
1.组合TcpServer对象
2.创建EventLoop事件循环对象的指针
3.明确TcpServer构造函数需要什么参数，输出ChatServer的构造函数
4.在当前服务器类的构造函数当中，注册处理连接的回调函数和处理读写事件的回调函数
5.设置合适的服务端线程数量，muduo库会自己划分IO线程和Worker线程
*/
class ChatServer
{
public:
    ChatServer(EventLoop *loop, //事件循环
                const InetAddress &listenAddr,//IP + Port
                const string &nameArg) //服务器的名字
            :_server(loop,listenAddr,nameArg),_loop(loop)
        {
            //给服务器注册用户连接的创建和断开回调
            _server.setConnectionCallback(std::bind(&ChatServer::onConnection,this,_1));

            //给服务器注册用户读写事件回调
            _server.setMessageCallback(std::bind(&ChatServer::onMessage,this,_1,_2,_3));

            //设置服务器端的线程数量  1个I/O线程，3个Worker线程
            _server.setThreadNum(4);
        }

        //开启事件循环
        void start()
        {
            _server.start();
        }
    

private:
    //专门处理用户的连接创建和断开  epoll listenfd accept
    void onConnection(const TcpConnectionPtr &conn)
    {
        if(conn->connected())
        {
            cout << conn->peerAddress().toIpPort() << "->" << 
            conn->localAddress().toIpPort() << " state:online"<< endl;
        }
        else
        {
            cout << conn->peerAddress().toIpPort() << "->" << 
            conn->localAddress().toIpPort() << " state:offline"<< endl;
            conn->shutdown(); //close(fd)
        }
        
    }
    void onMessage(const TcpConnectionPtr &conn,//连接（发送方）
                    Buffer *buffer,//缓冲区
                    Timestamp time)//接收到数据的时间信息
    {
        //1.把缓冲器里的所有数据拷贝成一个string返回
        //2.清空缓冲器，为下一次接收作准备
        string buf = buffer->retrieveAllAsString();
        cout << "recv data:" << buf << " time:" << time.toString() << endl;
        conn->send(buf);
    }
    TcpServer _server;
    EventLoop *_loop;
};

int main()
{
    //创建事件循环
    EventLoop loop;//epoll
    InetAddress addr("127.0.0.1",6000);//定义监听地址
    ChatServer server(&loop,addr,"ChatServer");

    server.start();//listenfd epool_ctl =>epoll
    loop.loop();//epoll_wait以阻塞方式等待新用户连接，已连接用户的读写事件等

    return 0;
}