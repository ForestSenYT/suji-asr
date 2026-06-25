#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
namespace suji {
template<class T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t cap): cap_(cap?cap:1) {}
  void push(T v){                                   // blocks while full and not closed
    std::unique_lock<std::mutex> lk(m_);
    notfull_.wait(lk, [&]{ return q_.size()<cap_ || closed_; });
    if(closed_) return;
    q_.push(std::move(v)); notempty_.notify_one();
  }
  bool pop(T& out){                                 // blocks for 1; false iff closed && empty
    std::unique_lock<std::mutex> lk(m_);
    notempty_.wait(lk, [&]{ return !q_.empty() || closed_; });
    if(q_.empty()) return false;
    out=std::move(q_.front()); q_.pop(); notfull_.notify_one(); return true;
  }
  bool try_pop(T& out){                              // non-blocking; false if empty
    std::lock_guard<std::mutex> lk(m_);
    if(q_.empty()) return false;
    out=std::move(q_.front()); q_.pop(); notfull_.notify_one(); return true;
  }
  void close(){ { std::lock_guard<std::mutex> lk(m_); closed_=true; } notempty_.notify_all(); notfull_.notify_all(); }
private:
  size_t cap_; std::queue<T> q_; std::mutex m_;
  std::condition_variable notempty_, notfull_; bool closed_=false;
};
}
