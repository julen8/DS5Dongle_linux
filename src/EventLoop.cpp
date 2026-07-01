#include "EventLoop.h"

#include <chrono>

EventLoop::EventLoop() { base = event_base_new(); }

EventLoop::~EventLoop() {
    if (base != nullptr) {
        event_base_free(base);
    }
}

bool EventLoop::valid() const { return base != nullptr; }

event* EventLoop::createFdEvent(const evutil_socket_t fd, short events, const bool persistent, const Callback callback, void* arg) const {
    if (base == nullptr) {
        return nullptr;
    }
    if (persistent) {
        events |= EV_PERSIST;
    }
    return event_new(base, fd, events, callback, arg);
}

event* EventLoop::createTimerEvent(const bool persistent, const Callback callback, void* arg) const {
    return createFdEvent(-1, 0, persistent, callback, arg);
}

bool EventLoop::addEvent(event* ev, const timeval* timeout) const { return ev != nullptr && event_add(ev, timeout) == 0; }

void EventLoop::freeEvent(event* ev) const {
    if (ev != nullptr) {
        event_free(ev);
    }
}

int EventLoop::dispatch() const {
    if (base == nullptr) {
        return -1;
    }
    return event_base_dispatch(base);
}

int EventLoop::runOnce() const {
    if (base == nullptr) {
        return -1;
    }
    return event_base_loop(base, EVLOOP_ONCE);
}

void EventLoop::onLoopTimeout(evutil_socket_t, short, void* arg) {
    auto* loop = static_cast<EventLoop*>(arg);
    if (loop != nullptr) {
        loop->breakLoop();
    }
}

int EventLoop::runOnceWithTimeoutMs(const int timeoutMs) const {
    if (base == nullptr) {
        return -1;
    }

    event* timeoutEvent = nullptr;
    if (timeoutMs >= 0) {
        timeoutEvent = event_new(base, -1, 0, &EventLoop::onLoopTimeout, const_cast<EventLoop*>(this));
        if (timeoutEvent == nullptr) {
            return -1;
        }
        const auto timeout = std::chrono::milliseconds(timeoutMs);
        const timeval tv{.tv_sec = static_cast<time_t>(timeout.count() / 1000),
                         .tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
        if (event_add(timeoutEvent, &tv) != 0) {
            event_free(timeoutEvent);
            return -1;
        }
    }

    const int ret = event_base_loop(base, EVLOOP_ONCE);

    if (timeoutEvent != nullptr) {
        event_free(timeoutEvent);
    }
    return ret;
}

void EventLoop::breakLoop() const {
    if (base != nullptr) {
        event_base_loopbreak(base);
    }
}
