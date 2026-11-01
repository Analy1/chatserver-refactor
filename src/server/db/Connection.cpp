#include "Connection.h"

#include <iostream>

Connection::Connection()
    : _conn(mysql_init(nullptr))
{
}

Connection::~Connection()
{
    if (_conn != nullptr)
    {
        mysql_close(_conn);
    }
}

bool Connection::connect(const std::string &ip, unsigned short port,
                         const std::string &user, const std::string &password,
                         const std::string &dbname)
{
    MYSQL *result = mysql_real_connect(_conn, ip.c_str(), user.c_str(),
                                       password.c_str(), dbname.c_str(),
                                       port, nullptr, 0);
    if (result == nullptr)
    {
        std::cerr << "connect mysql fail: " << mysql_error(_conn) << std::endl;
        return false;
    }

    // 数据库是 utf8mb4，连接也用 utf8mb4
    mysql_set_character_set(_conn, "utf8mb4");
    refreshAliveTime();
    return true;
}

bool Connection::update(const std::string &sql)
{
    if (mysql_query(_conn, sql.c_str()) != 0)
    {
        std::cerr << "update fail: " << mysql_error(_conn) << std::endl;
        return false;
    }
    return true;
}

MYSQL_RES *Connection::query(const std::string &sql)
{
    if (mysql_query(_conn, sql.c_str()) != 0)
    {
        std::cerr << "query fail: " << mysql_error(_conn) << std::endl;
        return nullptr;
    }
    return mysql_use_result(_conn);
}

void Connection::refreshAliveTime()
{
    _lastActiveTime = std::chrono::steady_clock::now();
}

long long Connection::idleMilliseconds() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - _lastActiveTime)
        .count();
}

MYSQL *Connection::getConnection()
{
    return _conn;
}
