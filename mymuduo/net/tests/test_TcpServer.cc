#include "mymuduo/net/TcpConnection.h"
#include "mymuduo/net/EventLoop.h"
#include "mymuduo/net/TcpServer.h"
#include "mymuduo/base/Logging.h"

using namespace mymuduo;
using namespace mymuduo::net;

void onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        printf("onConnection: new connection [%s] from %s\n",
               conn->name().c_str(),
               conn->peerAddr().toIpPort().c_str());
    }
    else
    {
        printf("onConnection(): connection [%s] is down\n",
               conn->name().c_str());
    }
}

void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp receiveTime)
{
    string msg = buf->retrieveAllAsString();
    printf("on message: %s", msg.c_str());
    conn->send(msg);
}

int main(int argc, char const *argv[])
{
    Logger::setLogLevel(Logger::TRACE);
    printf("main(): pid = %d", getpid());

    InetAddress listenAddr(9981);
    EventLoop loop;
    std::string name = "main";
    TcpServer server(&loop, listenAddr, name);
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);
    server.setThreadNum(5);
    server.start();
    loop.loop();
    return 0;
}
