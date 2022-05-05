#include "EventLoop.h"
#include "Channel.h"
#include "Poller.h"
#include "TimerId.h"
#include "TimerQueue.h"
#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Timestamp.h>
#include <cstdio>
#include <cassert>
#include <poll.h>
#include <stdio.h>

using namespace mymuduo;

using TimerCallback = muduo::net::TimerCallback;

__thread EventLoop *t_loopInThisThread = 0;

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      threadId_(muduo::CurrentThread::tid()),
      poller_(new Poller(this)),
      timerQueue_(new mymuduo::TimerQueue(this))
{
    printf("EventLoop created in %d\n", threadId_);
    // 一个线程一个 EventLoop，所以不存在线程安全的问题
    if (t_loopInThisThread)
    {
        printf("Another EventLoop has exists in thread %d\n", threadId_);
        exit(1);
    }
    else
    {
        t_loopInThisThread = this;
    }
}

EventLoop::~EventLoop()
{
    assert(!looping_);
    t_loopInThisThread = NULL;
}

EventLoop *EventLoop::getEventLoopOfCurrentThread()
{
    return t_loopInThisThread;
}

/**
 * 调用Poller::poll()获得当 前活动事件的Channel列表，然后依次调用每个Channel的handleEvent() 函数
 */
void EventLoop::loop()
{
    // 事件循环必须在IO线程执行，因此EventLoop::loop()会检查这一 pre-condition
    assert(!looping_);
    assertInLoopThread();
    looping_ = true;
    quit_ = false;

    while (!quit_)
    {
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        for (ChannelList::iterator it = activeChannels_.begin(); it != activeChannels_.end(); ++it)
        {
            (*it)->handleEvent();
        }
        doPendingFunctors();
    }

    printf("EventLoop stop looping\n");
    looping_ = false;
}

/**
 * 检查断言之后调用 Poller::updateChannel()，EventLoop不关心Poller是如何管理Channel列表 的
 */
void EventLoop::updateChannel(Channel *channel)
{
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
    printf("EventLoop::abortNotInLoopThread - EventLoop was created in threadId_ = \
%d , but current thread id = %d\n",
           threadId_, muduo::CurrentThread::tid());
}

TimerId EventLoop::runAt(const Timestamp &time, const TimerCallback &cb)
{
    return timerQueue_->addTimer(cb, time, 0.0);
}
TimerId EventLoop::runAfter(double delay, const TimerCallback &cb)
{
    Timestamp time(muduo::addTime(Timestamp::now(), delay));
    return runAt(time, cb);
}
TimerId EventLoop::runEvery(double interval, const TimerCallback &cb)
{
    Timestamp time(muduo::addTime(Timestamp::now(), interval));
    return timerQueue_->addTimer(cb, time, interval);
}
void EventLoop::cancel(TimerId timerId)
{
    timerQueue_->cancel(timerId);
}

void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread())
    {
        cb();
    }
    else
    {
        // 不在创建的线程上，先加入队列
        queueInLoop(std::move(cb));
    }
}

/**
 * 在它的IO线程内执行某个用户 任务回调，即EventLoop::runInLoop(const Functor& cb)
 * 其中Functor是 boost::function<void()>。如果用户在当前IO线程调用这个函数,
 * 回调会同步进行；如果用户在其他线程调用runInLoop()，cb会被加入队列，
 * IO 线程会被唤醒来调用这个Functor
 */
void EventLoop::queueInLoop(Functor cb)
{
    {
        muduo::MutexLockGuard lock(mutex_);
        pendingFunctors_.push_back(cb);
    }
    // 如果调用queueInLoop()的线程不是IO线程， 那么唤醒是必需的；
    // 如果在IO线程调用queueInLoop()，而此时正在调用 pending functor，那么也必须唤醒
    if (!isInLoopThread() || callingPendingFunctors_)
    {
        wakeup();
    }
}

/**
 * EventLoop::doPendingFunctors()不是简单地在临界区内依次调用 Functor，
 * 而是把回调列表swap()到局部变量functors中，这样一方面减小了临界区的长度
 * （意味着不会阻塞其他线程调用queueInLoop()），
 * 另一方面也避免了死锁（因为Functor可能再调用queueInLoop()）
 */
void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        /**
         * 由于doPendingFunctors()调用的Functor可能再调用 queueInLoop(cb)，
         * 这时queueInLoop()就必须wakeup()，否则这些新加的 cb就不能被及时调用了
         */
        muduo::MutexLockGuard lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (size_t i = 0; i < functors.size(); ++i)
    {
        functors[i]();
    }
    callingPendingFunctors_ = false;
}

void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = write(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        printf("EventLoop::wakeup() writes %ld bytes instead of 8", n);
    }
}

void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = read(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        printf("EventLoop::handleRead() reads %ld bytes instead of 8", n);
    }
}

void EventLoop::quit()
{
    // 在必要时唤醒IO线程，让它及时终止循环
    quit_ = true;
    if (!isInLoopThread())
    {
        wakeup();
    }
}
