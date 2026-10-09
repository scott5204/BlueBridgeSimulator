#include "debug/ipc/DebugCommand.h"

#include <chrono>

// ---------------------------------------------------------------------------
// CommandSlot
// ---------------------------------------------------------------------------
void CommandSlot::complete(bbipc::WireStatus status,
                           std::vector<uint8_t> payload) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;  // first completion wins
        status_ = status;
        payload_ = std::move(payload);
        done_ = true;
    }
    cv_.notify_all();
}

bool CommandSlot::wait(std::vector<uint8_t>& payload, bbipc::WireStatus& status,
                       int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                      [this] { return done_ || abandoned_; })) {
        return false;  // timeout -- caller abandons
    }
    if (abandoned_) return false;
    status = status_;
    payload = payload_;
    return true;
}

void CommandSlot::abandon() {
    {
        std::lock_guard<std::mutex> lk(m_);
        abandoned_ = true;
    }
    cv_.notify_all();
}

bool CommandSlot::abandoned() const {
    std::lock_guard<std::mutex> lk(m_);
    return abandoned_;
}

bool CommandSlot::done() const {
    std::lock_guard<std::mutex> lk(m_);
    return done_;
}

// ---------------------------------------------------------------------------
// priority class
// ---------------------------------------------------------------------------
bool isControlOpcode(uint16_t opcode) {
    switch (opcode) {
    case bbipc::kOpHello:
    case bbipc::kOpHalt:
    case bbipc::kOpResume:
    case bbipc::kOpStep:
    case bbipc::kOpResetHalt:
    case bbipc::kOpResetRun:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// DebugCommandQueue
// ---------------------------------------------------------------------------
void DebugCommandQueue::push(DebugCommand cmd) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (cmd.control)
            control_.push_back(std::move(cmd));
        else
            normal_.push_back(std::move(cmd));
    }
    cv_.notify_all();
}

bool DebugCommandQueue::tryPop(DebugCommand& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (!control_.empty()) {
        out = std::move(control_.front());
        control_.pop_front();
        return true;
    }
    if (!normal_.empty()) {
        out = std::move(normal_.front());
        normal_.pop_front();
        return true;
    }
    return false;
}

void DebugCommandQueue::waitForWork(int timeoutMs) const {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [this] { return !control_.empty() || !normal_.empty(); });
}

size_t DebugCommandQueue::pending() const {
    std::lock_guard<std::mutex> lk(m_);
    return control_.size() + normal_.size();
}

void DebugCommandQueue::clear() {
    std::deque<DebugCommand> dropped;
    {
        std::lock_guard<std::mutex> lk(m_);
        dropped.swap(control_);
        for (auto& c : normal_) dropped.push_back(std::move(c));
        normal_.clear();
    }
    // Never let a worker that is still waiting see "abandoned" as a success.
    for (auto& c : dropped) {
        if (c.slot) c.slot->abandon();
    }
}