#ifndef MY_ACCEPTOR_H
#define MY_ACCEPTOR_H

#include "noncopyable.h"
#include "Channel.h"
#include "Socket.h"

#include <boost/function.hpp>

namespace mymuduo
{
    namespace net
    {
        class EventLoop;
        class InetAddress;

        /**
         * 用于accept(2)新TCP连接，并通过回调通知使用者。
         * 它是内部class，供TcpServer使用，生命期由后者控制。
         */
        class Acceptor : noncopyable
        {
        public:
            typedef boost::function<void(int sockfd, const InetAddress &)> NewConnectionCallback;

            Acceptor(EventLoop *loop, const InetAddress &listenAddr);
            ~Acceptor();

            void setNewConnectionCallback(const NewConnectionCallback &cb)
            {
                newConnectionCallback_ = cb;
            };
            bool listening() const { return listening_; }
            void listen();

        private:
            // readable 回调函数，用于accept连接
            void handleRead();
            bool listening_;

            EventLoop *loop_;
            // RAII handle 封装了socket的生命周期
            Socket acceptSocket_;
            // 观察socket上的readable事件
            Channel acceptChannel_;
            // 用户回调函数，accept之后调用
            NewConnectionCallback newConnectionCallback_;
        };
    }
}

#endif // !MY_ACCEPTOR_H