#pragma once

#include <queue>
#include <unordered_map>
#include <chrono>
#include <functional>
#include <vector>

using namespace std; // 在头文件中使用虽然不严谨，但为了和你之前的代码保持一致，方便编译

typedef function<void()> TimeoutCallBack;
typedef chrono::high_resolution_clock Clock;
typedef chrono::milliseconds MS;
typedef Clock::time_point Timestamp;

struct TimerNode {
    int id;             
    Timestamp expires;  
    TimeoutCallBack cb; 
    
    bool operator>(const TimerNode& t) const {
        return expires > t.expires;
    }
};

class TimerManager {
public:
    TimerManager() { }

    void addTimer(int id, int timeoutMS, const TimeoutCallBack& cb) {
        Timestamp expires = Clock::now() + MS(timeoutMS);
        heap_.push({id, expires, cb});
        ref_[id] = expires; 
    }

    void tick() {
        while (!heap_.empty()) {
            TimerNode node = heap_.top();
            if (Clock::now() < node.expires) break;

            if (ref_.count(node.id) && ref_[node.id] == node.expires) {
                node.cb();
                ref_.erase(node.id);
            }
            heap_.pop();
        }
    }

    int getNextTick() {
        tick();
        if (heap_.empty()) return -1;
        auto res = chrono::duration_cast<MS>(heap_.top().expires - Clock::now());
        int result = (int)res.count();
        return result > 0 ? result : 0;
    }

    void delTimer(int id) { ref_.erase(id); }

private:
    priority_queue<TimerNode, vector<TimerNode>, greater<TimerNode>> heap_;
    unordered_map<int, Timestamp> ref_;
};