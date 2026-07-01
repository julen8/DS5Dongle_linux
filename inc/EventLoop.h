#pragma once

#include <event2/event.h>

class EventLoop {
public:
    using Callback = void (*)(evutil_socket_t, short, void*);

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool valid() const;
    event* createFdEvent(evutil_socket_t fd, short events, bool persistent, Callback callback, void* arg) const;
    event* createTimerEvent(bool persistent, Callback callback, void* arg) const;
    bool addEvent(event* ev, const timeval* timeout) const;
    void freeEvent(event* ev) const;

    int dispatch() const;
    int runOnce() const;
    int runOnceWithTimeoutMs(int timeoutMs) const;
    void breakLoop() const;

private:
    static void onLoopTimeout(evutil_socket_t, short, void* arg);

    event_base* base = nullptr;
};
