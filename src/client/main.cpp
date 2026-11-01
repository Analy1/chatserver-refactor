#include "json.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <functional>
using namespace std;
using json = nlohmann::json;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <atomic>
#include <poll.h>
#include <cerrno>
#include <cstring>

#include "group.hpp"
#include "user.hpp"
#include "public.hpp"
#include "net/frame_codec.hpp"

// 记录当前系统登录的用户信息
User g_currentUser;
// 记录当前登录用户的好友列表信息
vector<User> g_currentUserFriendList;
// 记录当前登录用户的群组列表信息
vector<Group> g_currentUserGroupList;

// 控制主菜单页面程序
bool isMainMenuRunning = false;

// 用于读写线程之间的通信
sem_t rwsem;
// 记录登录状态
atomic_bool g_isLoginSuccess{false};
// 服务端连接是否已断开（接收线程置位，主线程据此退出）
atomic_bool g_connClosed{false};


// 接收线程
void readTaskHandler(int clientfd);
// 获取系统时间（聊天信息需要添加时间信息）
string getCurrentTime();
// 主聊天页面程序
void mainMenu(int);
// 显示当前登录成功用户的基本信息
void showCurrentUserData();

// 聊天客户端程序实现，main线程用作发送线程，子线程用作接收线程
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << endl;
        exit(-1);
    }

    // 解析通过命令行参数传递的ip和port
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    // 创建client端的socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd)
    {
        cerr << "socket create error" << endl;
        exit(-1);
    }

    // 填写client需要连接的server信息ip+port
    sockaddr_in server;
    memset(&server, 0, sizeof(sockaddr_in));

    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip);

    // client和server进行连接
    if (-1 == connect(clientfd, (sockaddr *)&server, sizeof(sockaddr_in)))
    {
        cerr << "connect server error" << endl;
        close(clientfd);
        exit(-1);
    }

    // 初始化读写线程通信用的信号量
    sem_init(&rwsem, 0, 0);

    // 连接服务器成功，启动接收子线程
    std::thread readTask(readTaskHandler, clientfd); // pthread_create
    readTask.detach();                               // pthread_detach

    // main线程用于接收用户输入，负责发送数据
    for (;;)
    {
        if (g_connClosed)
        {
            cerr << "connection closed by server" << endl;
            exit(0);
        }
        // 显示首页面菜单 登录、注册、退出
        cout << "========================" << endl;
        cout << "1. login" << endl;
        cout << "2. register" << endl;
        cout << "3. quit" << endl;
        cout << "========================" << endl;
        cout << "choice:";
        int choice = 0;
        cin >> choice;
        cin.get(); // 读掉缓冲区残留的回车

        switch (choice)
        {
        case 1: // login业务
        {
            int id = 0;
            char pwd[50] = {0};
            cout << "userid:";
            cin >> id;
            cin.get(); // 读掉缓冲区残留的回车
            cout << "userpassword:";
            cin.getline(pwd, 50);

            json js;
            js["msgid"] = LOGIN_MSG;
            js["id"] = id;
            js["password"] = pwd;

            g_isLoginSuccess = false;

            if (!frame::sendJson(clientfd, js))
            {
                cerr << "send login msg error:" << js.dump() << endl;
            }

            sem_wait(&rwsem); // 等待信号量，由子线程处理完登录的响应消息后，通知这里

            if (g_connClosed)
            {
                cerr << "connection closed by server" << endl;
                exit(0);
            }

            if (g_isLoginSuccess) 
            {
                // 进入聊天主菜单页面
                isMainMenuRunning = true;
                mainMenu(clientfd);
            }
        }
        break;
        case 2: // register业务
        {
            char name[50] = {0};
            char pwd[50] = {0};
            cout << "username:";
            cin.getline(name, 50);
            cout << "userpassword:";
            cin.getline(pwd, 50);

            json js;
            js["msgid"] = REG_MSG;
            js["name"] = name;
            js["password"] = pwd;

            if (!frame::sendJson(clientfd, js))
            {
                cerr << "send reg msg error:" << js.dump() << endl;
            }
            
            sem_wait(&rwsem); // 等待信号量，子线程处理完注册消息会通知

            if (g_connClosed)
            {
                cerr << "connection closed by server" << endl;
                exit(0);
            }
        }
        break;
        case 3: // quit业务
            close(clientfd);
            sem_destroy(&rwsem);
            exit(0);
        default:
            cerr << "invalid input!" << endl;
            break;
        }
    }

    return 0;
}

// 处理注册的响应逻辑
void doRegResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>()) // 注册失败
    {
        cerr << "name is already exist, register error!" << endl;
    }
    else // 注册成功
    {
        cout << "name register success, userid is " << responsejs["id"]
                << ", do not forget it!" << endl;
    }
}

// 处理登录的响应逻辑
void doLoginResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>()) // 登录失败
    {
        cerr << responsejs["errmsg"] << endl;
        g_isLoginSuccess = false;
    }
    else // 登录成功
    {
        // 记录当前用户的id和name
        g_currentUser.setId(responsejs["id"].get<int>());
        g_currentUser.setName(responsejs["name"]);

        // 记录当前用户的好友列表信息
        if (responsejs.contains("friends"))
        {
            // 初始化
            g_currentUserFriendList.clear();

            vector<string> vec = responsejs["friends"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                User user;
                user.setId(js["id"].get<int>());
                user.setName(js["name"]);
                user.setState(js["state"]);
                g_currentUserFriendList.push_back(user);
            }
        }

        // 记录当前用户的群组列表信息
        if (responsejs.contains("groups"))
        {
            // 初始化
            g_currentUserGroupList.clear();

            vector<string> vec1 = responsejs["groups"];
            for (string &groupstr : vec1)
            {
                json grpjs = json::parse(groupstr);
                Group group;
                group.setId(grpjs["id"].get<int>());
                group.setName(grpjs["groupname"]);
                group.setDesc(grpjs["groupdesc"]);

                vector<string> vec2 = grpjs["users"];
                for (string &userstr : vec2)
                {
                    GroupUser user;
                    json js = json::parse(userstr);
                    user.setId(js["id"].get<int>());
                    user.setName(js["name"]);
                    user.setState(js["state"]);
                    user.setRole(js["role"]);
                    group.getUsers().push_back(user);
                }

                g_currentUserGroupList.push_back(group);
            }
        }

        // 显示登录用户的基本信息
        showCurrentUserData();

        // 显示当前用户的离线消息  个人聊天信息或者群组消息
        if (responsejs.contains("offlinemsg"))
        {
            vector<string> vec = responsejs["offlinemsg"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                // time + [id] + name + " said: " + xxx
                if (ONE_CHAT_MSG == js["msgid"].get<int>())
                {
                    cout << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
                            << " said: " << js["msg"].get<string>() << endl;
                }
                else
                {
                    cout << "群消息[" << js["groupid"] << "]:" << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
                            << " said: " << js["msg"].get<string>() << endl;
                }
            }
        }

        g_isLoginSuccess = true;
    }
}

// 处理一条已经从网络收到、并且已经切好帧的消息。
// 参数 js：这条消息解析出来的 JSON 对象，靠里面的 "msgid" 判断是哪种消息。
// 返回值：true = 正常处理完了，可以继续收下一条；
//         false = 消息内容不合法，接收线程应该断开连接。
bool handleServerMessage(json &js)
{
    if (!js.is_object() || !js.contains("msgid") || !js["msgid"].is_number_integer())
    {
        cerr << "message without valid msgid" << endl;
        return false;
    }

    // msgtype：消息编号。取值见 include/public.hpp：
    // 1=登录请求，2=登录响应，3=注销，4=注册请求，5=注册响应，
    // 6=单聊，7=加好友，8=建群，9=加群，10=群聊
    int msgtype = js["msgid"].get<int>();
    if (ONE_CHAT_MSG == msgtype)
    {
        cout << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
             << " said: " << js["msg"].get<string>() << endl;
        return true;
    }

    if (GROUP_CHAT_MSG == msgtype)
    {
        cout << "群消息[" << js["groupid"] << "]:" << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
             << " said: " << js["msg"].get<string>() << endl;
        return true;
    }

    if (LOGIN_MSG_ACK == msgtype)
    {
        doLoginResponse(js); // 处理登录响应的业务逻辑
        sem_post(&rwsem);    // 通知主线程，登录结果处理完成
        return true;
    }

    if (REG_MSG_ACK == msgtype)
    {
        doRegResponse(js);
        sem_post(&rwsem);    // 通知主线程，注册结果处理完成
        return true;
    }

    return true;
}

// 接收线程：只负责"从 socket 收字节"和"把字节切成一条条完整消息"。
// 切出来的每条消息交给 handleServerMessage 处理（单聊/群聊打印、登录响应等）。
void readTaskHandler(int clientfd)
{
    // inbuf：存放"已经收到、但还没凑成一条完整消息"的字节。
    //        每次 recv 的新字节都追加到它后面；每成功切出一条完整消息，
    //        就把这条消息占用的字节从 inbuf 前面删掉。
    string inbuf;
    inbuf.reserve(8192);

    // buf：每次调用 recv 时的临时接收区，最多装 8192 字节。
    //      它只是"这一次从操作系统读出来的数据"，可能是一条、半条或多条消息。
    char buf[8192];

    for (;;)
    {
        // pollfd：告诉 poll 我们要监视哪个 socket、关心什么事件。
        struct pollfd pfd;
        pfd.fd = clientfd;      // 监视客户端 socket
        pfd.events = POLLIN;    // 关心"有数据可读"
        pfd.revents = 0;        // 由 poll 填写实际发生了什么，先清零

        // rc：poll 的返回值。
        //     > 0：有数据可读
        //     = 0：等了 500ms 仍然没有数据（超时）
        //     < 0：出错
        int rc = 0;
        do
        {
            rc = poll(&pfd, 1, 500);
        } while (rc < 0 && errno == EINTR);  // 被信号打断就重试

        if (rc < 0)
        {
            cerr << "poll error: " << strerror(errno) << endl;
            break;
        }
        if (rc == 0)
        {
            continue;  // 500ms 内没有新数据，回到循环开头继续等
        }

        // n：这次 recv 实际读到的字节数。
        //    > 0：读到了 n 个字节
        //    = 0：对方关闭了连接
        //    < 0：出错
        ssize_t n = recv(clientfd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            // EINTR：被信号打断；EAGAIN/EWOULDBLOCK：暂时没有数据。
            // 这两种都不是真错误，重试即可。
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            cerr << "recv error: " << strerror(errno) << endl;
            break;
        }
        if (n == 0)
        {
            // 对方关闭连接。如果 inbuf 里还剩着字节，说明最后一条消息没发完整。
            if (!inbuf.empty())
            {
                cerr << "connection closed with truncated frame, "
                     << inbuf.size() << " bytes left" << endl;
            }
            break;
        }

        // 把这次读到的 n 个字节接到 inbuf 末尾，和之前没凑齐的字节拼在一起。
        inbuf.append(buf, static_cast<size_t>(n));

        // offset：这一轮已经处理到 inbuf 的第几个字节。
        //         每成功取出一条完整消息，offset 就往后移动这条消息的长度。
        size_t offset = 0;

        // fatal：是否遇到了无法继续处理的错误（消息格式非法）。
        //        为 true 时跳出外层循环，结束接收线程。
        bool fatal = false;

        // 反复从 inbuf 里取消息，直到剩下的字节不够一条完整消息为止。
        while (true)
        {
            // body：从 inbuf 里切出来的一条完整消息的正文（JSON 字符串），
            //       还没有解析成 json 对象。
            string body;

            // consumed：extract 返回的"这条消息一共占了多少字节"，
            //           包含 4 字节长度头和后面的 JSON 正文。
            size_t consumed = 0;

            // status：extract 的结果。
            frame::Status status = frame::extract(inbuf.data() + offset,
                                                  inbuf.size() - offset,
                                                  body, consumed);

            if (status == frame::Status::NeedMore)
            {
                // 数据不够一条完整消息，先不处理。等下一次 recv 补上后再循环。
                break;
            }
            if (status != frame::Status::Ok)
            {
                // 长度头不合法，这条连接上的数据已经无法再对齐，只能断开。
                cerr << "protocol error, status=" << static_cast<int>(status) << endl;
                fatal = true;
                break;
            }

            // 这条消息已经完整取出来了，下次从它后面继续取。
            offset += consumed;

            try
            {
                // js：把 JSON 字符串解析成对象，后面按 msgid 判断消息类型。
                json js = json::parse(body);

                // 交给业务层处理。返回 false 表示消息内容有问题，需要断开连接。
                if (!handleServerMessage(js))
                {
                    fatal = true;
                }
            }
            catch (const json::exception &e)
            {
                // JSON 内容本身坏了（比如不是合法 JSON），断开当前连接。
                cerr << "invalid message: " << e.what() << endl;
                fatal = true;
            }

            if (fatal)
            {
                break;
            }
        }

        // 把这一轮已经处理过的字节从 inbuf 前面删掉，
        // 剩下的就是"还不够一条完整消息"的尾巴，留给下一次 recv 拼接。
        if (offset > 0)
        {
            inbuf.erase(0, offset);
        }

        if (fatal)
        {
            break;
        }
    }

    // 走到这里说明接收线程要结束了（对端关闭或协议错误）。
    // g_connClosed：告诉主线程"连接已经断了"，让主线程不要再等用户输入。
    g_connClosed = true;
    isMainMenuRunning = false;

    // sem_post：唤醒主线程。
    // 主线程登录/注册时会 sem_wait 等待服务端响应；如果此时不唤醒，
    // 它会一直卡在那里，即使连接已经断了也不知道。
    sem_post(&rwsem);

    // 关闭 socket。
    close(clientfd);
}

// 显示当前登录成功用户的基本信息
void showCurrentUserData()
{
    cout << "======================login user======================" << endl;
    cout << "current login user => id:" << g_currentUser.getId() << " name:" << g_currentUser.getName() << endl;
    cout << "----------------------friend list---------------------" << endl;
    if (!g_currentUserFriendList.empty())
    {
        for (User &user : g_currentUserFriendList)
        {
            cout << user.getId() << " " << user.getName() << " " << user.getState() << endl;
        }
    }
    cout << "----------------------group list----------------------" << endl;
    if (!g_currentUserGroupList.empty())
    {
        for (Group &group : g_currentUserGroupList)
        {
            cout << group.getId() << " " << group.getName() << " " << group.getDesc() << endl;
            for (GroupUser &user : group.getUsers())
            {
                cout << user.getId() << " " << user.getName() << " " << user.getState()
                     << " " << user.getRole() << endl;
            }
        }
    }
    cout << "======================================================" << endl;
}

// "help" command handler
void help(int fd = 0, string str = "");
// "chat" command handler
void chat(int, string);
// "addfriend" command handler
void addfriend(int, string);
// "creategroup" command handler
void creategroup(int, string);
// "addgroup" command handler
void addgroup(int, string);
// "groupchat" command handler
void groupchat(int, string);
// "loginout" command handler
void loginout(int, string);

// 系统支持的客户端命令列表
unordered_map<string, string> commandMap = {
    {"help", "显示所有支持的命令，格式help"},
    {"chat", "一对一聊天，格式chat:friendid:message"},
    {"addfriend", "添加好友，格式addfriend:friendid"},
    {"creategroup", "创建群组，格式creategroup:groupname:groupdesc"},
    {"addgroup", "加入群组，格式addgroup:groupid"},
    {"groupchat", "群聊，格式groupchat:groupid:message"},
    {"loginout", "注销，格式loginout"}};

// 注册系统支持的客户端命令处理
unordered_map<string, function<void(int, string)>> commandHandlerMap = {
    {"help", help},
    {"chat", chat},
    {"addfriend", addfriend},
    {"creategroup", creategroup},
    {"addgroup", addgroup},
    {"groupchat", groupchat},
    {"loginout", loginout}};

// 主聊天页面程序
void mainMenu(int clientfd)
{
    help();

    char buffer[1024] = {0};
    while (isMainMenuRunning)
    {
        if (g_connClosed)
        {
            break;
        }
        cin.getline(buffer, 1024);
        string commandbuf(buffer);
        string command; // 存储命令
        int idx = commandbuf.find(":");
        if (-1 == idx)
        {
            command = commandbuf;
        }
        else
        {
            command = commandbuf.substr(0, idx);
        }
        auto it = commandHandlerMap.find(command);
        if (it == commandHandlerMap.end())
        {
            cerr << "invalid input command!" << endl;
            continue;
        }

        // 调用相应命令的事件处理回调，mainMenu对修改封闭，添加新功能不需要修改该函数
        it->second(clientfd, commandbuf.substr(idx + 1, commandbuf.size() - idx)); // 调用命令处理方法
    }
}

// "help" command handler
void help(int, string)
{
    cout << "show command list >>> " << endl;
    for (auto &p : commandMap)
    {
        cout << p.first << " : " << p.second << endl;
    }
    cout << endl;
}
// "addfriend" command handler
void addfriend(int clientfd, string str)
{
    int friendid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_FRIEND_MSG;
    js["id"] = g_currentUser.getId();
    js["friendid"] = friendid;

    if (!frame::sendJson(clientfd, js))
    {
        cerr << "send addfriend msg error -> " << js.dump() << endl;
    }
}
// "chat" command handler
void chat(int clientfd, string str)
{
    int idx = str.find(":"); // friendid:message
    if (-1 == idx)
    {
        cerr << "chat command invalid!" << endl;
        return;
    }

    int friendid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);

    json js;
    js["msgid"] = ONE_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["toid"] = friendid;
    js["msg"] = message;
    js["time"] = getCurrentTime();

    if (!frame::sendJson(clientfd, js))
    {
        cerr << "send chat msg error -> " << js.dump() << endl;
    }
}
// "creategroup" command handler  groupname:groupdesc
void creategroup(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx)
    {
        cerr << "creategroup command invalid!" << endl;
        return;
    }

    string groupname = str.substr(0, idx);
    string groupdesc = str.substr(idx + 1, str.size() - idx);

    json js;
    js["msgid"] = CREATE_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupname"] = groupname;
    js["groupdesc"] = groupdesc;

    if (!frame::sendJson(clientfd, js))
    {
        cerr << "send creategroup msg error -> " << js.dump() << endl;
    }
}
// "addgroup" command handler
void addgroup(int clientfd, string str)
{
    int groupid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupid"] = groupid;

    if (!frame::sendJson(clientfd, js))
    {
        cerr << "send addgroup msg error -> " << js.dump() << endl;
    }
}
// "groupchat" command handler   groupid:message
void groupchat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx)
    {
        cerr << "groupchat command invalid!" << endl;
        return;
    }

    int groupid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);

    json js;
    js["msgid"] = GROUP_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["groupid"] = groupid;
    js["msg"] = message;
    js["time"] = getCurrentTime();

    if (!frame::sendJson(clientfd, js))
    {
        cerr << "send groupchat msg error -> " << js.dump() << endl;
    }
}
// "loginout" command handler
void loginout(int clientfd, string)
{
    json js;
    js["msgid"] = LOGINOUT_MSG;
    js["id"] = g_currentUser.getId();

    if (!frame::sendJson(clientfd, js))
    {
        cerr << "send loginout msg error -> " << js.dump() << endl;
    }
    else
    {
        isMainMenuRunning = false;
    }   
}

// 获取系统时间（聊天信息需要添加时间信息）
string getCurrentTime()
{
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm *ptm = localtime(&tt);
    char date[60] = {0};
    sprintf(date, "%d-%02d-%02d %02d:%02d:%02d",
            (int)ptm->tm_year + 1900, (int)ptm->tm_mon + 1, (int)ptm->tm_mday,
            (int)ptm->tm_hour, (int)ptm->tm_min, (int)ptm->tm_sec);
    return std::string(date);
}