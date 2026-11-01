#include "CommonConnectionPool.h"

#include <chrono>
#include <thread>

ConnectionPool *ConnectionPool::getConnectionPool()
{
    // C++11 保证局部静态变量初始化线程安全
    static ConnectionPool pool;
    return &pool;
}

ConnectionPool::ConnectionPool()
    : _ip("127.0.0.1"),
      _port(3306),
      _username("root"),
      _password("111111"),
      _dbname("chat"),
      _initSize(10),
      _maxSize(100),
      _maxIdleTime(60),
      _connectionTimeOut(1000),
      _connectionCnt(0)
{
    // 先创建初始连接
    for (int i = 0; i < _initSize; ++i)
    {
        Connection *conn = new Connection();
        if (conn->connect(_ip, _port, _username, _password, _dbname))
        {
            conn->refreshAliveTime();
            _connectionQue.push(conn);
            ++_connectionCnt;
        }
        else
        {
            delete conn;
        }
    }

    // 生产者线程：队列空了就补连接
    std::thread produce(&ConnectionPool::produceConnectionTask, this);
    produce.detach();

    // 扫描线程：回收空闲太久的连接
    std::thread scanner(&ConnectionPool::scannerConnectionTask, this);
    scanner.detach();
}

std::shared_ptr<Connection> ConnectionPool::getConnection()
{
    std::unique_lock<std::mutex> lock(_queueMutex);

    // 等一个空闲连接，最多等 _connectionTimeOut 毫秒
    bool hasConnection = _cv.wait_for(
        lock,
        std::chrono::milliseconds(_connectionTimeOut),
        [this]()
        { return !_connectionQue.empty(); });

    if (!hasConnection)
    {
        return nullptr;
    }

    Connection *rawConnection = _connectionQue.front();
    _connectionQue.pop();

    // 队列可能空了，唤醒生产者补充
    _cv.notify_all();

    // shared_ptr 析构时把连接放回队列
    return std::shared_ptr<Connection>(rawConnection, [this](Connection *conn)
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        conn->refreshAliveTime();
        _connectionQue.push(conn);
        _cv.notify_all();
    });
}

void ConnectionPool::produceConnectionTask()
{
    for (;;)
    {
        std::unique_lock<std::mutex> lock(_queueMutex);

        // 队列不空，或者已经到最大连接数，就等
        _cv.wait(lock, [this]()
                 { return _connectionQue.empty() && _connectionCnt < _maxSize; });

        ++_connectionCnt;
        lock.unlock(); // 建连接比较慢，不持锁

        Connection *conn = new Connection();
        if (!conn->connect(_ip, _port, _username, _password, _dbname))
        {
            delete conn;
            lock.lock();
            --_connectionCnt;
            continue;
        }

        lock.lock();
        conn->refreshAliveTime();
        _connectionQue.push(conn);
        _cv.notify_one();
    }
}

void ConnectionPool::scannerConnectionTask()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(_maxIdleTime));

        std::lock_guard<std::mutex> lock(_queueMutex);

        // 只回收超出初始连接数的空闲连接
        while (_connectionCnt > _initSize && !_connectionQue.empty())
        {
            Connection *conn = _connectionQue.front();
            if (conn->idleMilliseconds() < _maxIdleTime * 1000LL)
            {
                break; // 队首还没到空闲时间，后面的更不会
            }

            _connectionQue.pop();
            --_connectionCnt;
            delete conn;
        }
    }
}
