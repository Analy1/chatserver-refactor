#ifndef COMMON_CONNECTION_POOL_H
#define COMMON_CONNECTION_POOL_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include "Connection.h"

// MySQL 连接池：预建若干连接，业务层用完自动归还
class ConnectionPool
{
public:
    // 全局单例
    static ConnectionPool *getConnectionPool();

    // 借出一个连接；用完 shared_ptr 析构时自动归还
    std::shared_ptr<Connection> getConnection();

private:
    ConnectionPool();

    // 后台线程：空闲连接不够时补充
    void produceConnectionTask();

    // 后台线程：回收空闲太久的连接
    void scannerConnectionTask();

    std::string _ip;
    unsigned short _port;
    std::string _username;
    std::string _password;
    std::string _dbname;

    int _initSize;           // 初始连接数
    int _maxSize;            // 最大连接数
    int _maxIdleTime;        // 最大空闲时间（秒）
    int _connectionTimeOut;  // 借连接的超时时间（毫秒）

    std::queue<Connection *> _connectionQue; // 空闲连接队列
    std::mutex _queueMutex;                  // 保护队列
    std::condition_variable _cv;             // 通知有连接可用 / 队列为空
    int _connectionCnt;                      // 已创建的连接总数
};

#endif // COMMON_CONNECTION_POOL_H
