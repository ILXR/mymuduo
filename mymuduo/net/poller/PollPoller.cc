#include "mymuduo/net/poller/PollPoller.h"

#include "mymuduo/base/Types.h"
#include "mymuduo/base/Logging.h"
#include "mymuduo/net/Channel.h"

#include <algorithm>
#include <cassert>
#include <stdio.h>
#include <poll.h>

using namespace mymuduo;
using namespace mymuduo::net;

PollPoller::PollPoller(EventLoop *loop) : Poller(loop) {}

PollPoller::~PollPoller() = default;

/**
 * PollPoller::poll()是Poller的核心功能，它调用poll()获得当前活动的IO 事件，
 * 然后填充调用方传入的activeChannels，并返回poll() return的时刻。
 * 这里我们直接把vector<struct pollfd> pollfds_作为参数传给poll()，
 * 因为C++标准保证std::vector的元素排列跟数组一样。
 * &*pollfds_.begin()是获得元素的首地址，这个表达式的类型为 pollfds_*，
 * 符合poll()的要求。（在C++11中可写为pollfds_.data()， g++4.4的STL也支持这种写法。）
 */
Timestamp PollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    /* poll是linux的事件轮询机制函数，每个进程可以管理一个pollfd队列，由poll函数进行事件注册和查询
     * int poll(struct pollfd *fds, nfds_t nfds, int timeout);
     * struct pollfd {
     *   int   fd;         文件描述符
     *   short events;     events告诉poll监听fd上哪些事件，它是一系列事件按位或
     *   short revents;    由内核修改，来通知应用程序fd 上实际上发生了哪些事件
     * };
     * nfds -> pollfd队列长度
     * timeout -> poll的超时时间，单位毫秒. =-1时，poll永远阻塞，直到有事件发生. =0时，poll立即返回
     * 返回值 :
     *      >0  表示数组fds 中准备好读，写或出错状态的那些socket描述符的总数量
     *      ​==0 表示数组fds 中都没有准备好读写或出错，当poll 阻塞超时timeout 就会返回。
     *      -1  表示poll() 函数调用失败，同时回自动设置全局变量errno.
     */
    int numEvents = ::poll(&*pollfds_.begin(), pollfds_.size(), timeoutMs);
    int savedErrno = errno;
    Timestamp now(Timestamp::now());
    if (numEvents > 0)
    {
        LOG_TRACE << numEvents << " events happened";
        fillActiveChannels(numEvents, activeChannels);
    }
    else if (numEvents == 0)
    {
        // LOG_TRACE << " nothing happened";
    }
    else
    {
        if (savedErrno != EINTR)
        {
            errno = savedErrno;
            LOG_SYSERR << "PollPoller::poll()";
        }
    }
    return now;
}

/**
 * fillActiveChannels()遍历pollfds_，找出有活动事件的fd，把它对应 的Channel填入activeChannels
 * 为了提前结束循环，每找到一个 活动fd就递减numEvents，这样当numEvents减为0时表示活动fd都找完了
 * 当前活动事件revents会保存在Channel中，供 Channel::handleEvent()使用
 */
void PollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for (PollFdList::const_iterator pfd = pollfds_.begin(); pfd != pollfds_.end() && numEvents > 0; ++pfd)
    {
        if (pfd->revents > 0)
        {
            --numEvents;
            ChannelMap::const_iterator ch = channels_.find(pfd->fd);
            assert(ch != channels_.end());
            Channel *channel = ch->second;
            assert(channel->fd() == pfd->fd);
            channel->set_revents(pfd->revents);
            // pfd->revents == 0
            // Channel::handleEvent() 会添加或删除Channel，从而造成 pollfds_在遍历期间改变大小
            // 因此要在遍历完成后统一进行 handleEvent
            // 同时简化 PollPoller ，只负责IO multiplexing，不负责事件分发 （dispatching）。
            // 这样将来可以方便地替换为其他更高效的IO multiplexing机制，如epoll
            activeChannels->push_back(channel);
        }
    }
}

/**
 * PollPoller::updateChannel()的主要功能是负责维护和更新pollfds_数组。
 * 这里用了大量的 assert 来检查invariant
 */
void PollPoller::updateChannel(Channel *channel)
{
    assertInLoopThread();
    LOG_TRACE << "fd = " << channel->fd() << " events = " << channel->events();
    if (channel->index() < 0)
    {
        // 下标小于0，说明是新的Channel
        assert(channels_.find(channel->fd()) == channels_.end());
        struct pollfd pfd;
        pfd.fd = channel->fd();
        pfd.events = static_cast<short>(channel->events());
        pfd.revents = 0;
        pollfds_.push_back(pfd);
        int idx = static_cast<int>(pollfds_.size()) - 1; // 添加到了队列末尾
        channel->set_index(idx);
        channels_[pfd.fd] = channel; // 将 Channel 添加到map
    }
    else
    {
        // 已经存在的Channel
        assert(channels_.find(channel->fd()) != channels_.end());
        assert(channels_[channel->fd()] == channel);
        int idx = channel->index();
        assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
        struct pollfd &pfd = pollfds_[idx];
        assert(pfd.fd == channel->fd() || pfd.fd == -channel->fd() - 1);
        pfd.events = static_cast<short>(channel->events());
        pfd.revents = 0;
        // 如果某个Channel暂时不关心任何事件，就把pollfd.fd设 为-1，让poll()忽略此项
        // 如果待监测的 fd 为负值，则这个描述符的检测就会被忽略，poll()函数返回时直接把revents 设置为0
        // 这里不能改为把pollfd.events设为 0，这样无法屏蔽POLLER事件。
        // 改进的做法是把pollfd.fd设为channel->fd()的相反数减一，这样可以进一步检查invariant(不变量)
        if (channel->isNoneEvent()) // events == 0
        {
            // channel 中的fd永远是 >=0 ，但是pdf中的fd可正可负（负数时忽略此项）
            pfd.fd = -channel->fd() - 1;
        }
    }
}

void PollPoller::removeChannel(Channel *channel)
{
    assertInLoopThread();
    LOG_TRACE << "PollPoller::removeChannel fd = " << channel->fd();
    assert(channels_.find(channel->fd()) != channels_.end());
    assert(channels_[channel->fd()] == channel);
    assert(channel->isNoneEvent());
    int idx = channel->index();
    assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
    const struct pollfd &pfd = pollfds_[idx];
    (void)pfd;
    // 对应updateChannel中的改进，将 fd 取反减一
    assert(pfd.fd == -channel->fd() - 1 && pfd.events == channel->events());
    size_t n = channels_.erase(channel->fd());
    assert(n == 1);
    (void)n;
    if (implicit_cast<size_t>(idx) == pollfds_.size() - 1)
    {
        pollfds_.pop_back();
    }
    else
    {
        /**
         * 从数组pollfds_中删除元素是O(1)复杂度，办法是将待删除的元素与最后一个元素交换
         * 再pollfds_.pop_back()
         */
        int channelAdEnd = pollfds_.back().fd;
        std::iter_swap(pollfds_.begin() + idx, pollfds_.end() - 1);
        if (channelAdEnd < 0)
        {
            channelAdEnd = -channelAdEnd - 1;
        }
        channels_[channelAdEnd]->set_index(idx);
        pollfds_.pop_back();
    }
}