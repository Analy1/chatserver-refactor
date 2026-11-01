#ifndef CONNECTION_H
#define CONNECTION_H

#include <mysql/mysql.h>

#include <chrono>
#include <string>

// 一条 MySQL 连接：连接池里存放的对象
class Connection
{
public:
    Connection();
    ~Connection();

    // 建立连接
    bool connect(const std::string &ip, unsigned short port,
                 const std::string &user, const std::string &password,
                 const std::string &dbname);

    // 执行 insert / update / delete
    bool update(const std::string &sql);

    // 执行 select，返回结果集
    MYSQL_RES *query(const std::string &sql);

    // 刷新"最后一次使用时间"，用于空闲连接回收
    void refreshAliveTime();

    // 返回已经空闲了多少毫秒
    long long idleMilliseconds() const;

    // 返回底层 MYSQL 句柄，主要给 mysql_insert_id 用
    MYSQL *getConnection();

private:
    MYSQL *_conn; // MySQL 连接句柄
    std::chrono::steady_clock::time_point _lastActiveTime; // 最后一次使用时间
};

#endif // CONNECTION_H
