#ifndef C_THREAD_POOL_H
#define C_THREAD_POOL_H

#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <iostream>

#include "Locker.h"

using namespace std;
class ThreadPool {
public:
    using Thread = std::thread;
    using ThreadID = std::thread::id;
    using Task = std::function<void()>;

    static ThreadPool& instance() {
        static ThreadPool single;
        return single;
    }

    // 线程是绝不允许拷贝的 
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F, typename... T>
    auto submit(F&& func, T&&... params) -> std::future<typename std::invoke_result<F, T...>::type> {
        //首先用bind将函数名和参数绑定，然后用invoke_result::type（本来是result_of）得到返回的类型，
        //接着用packaged_task包装成PackagedTask,其可以异步执行任务
        auto execute = std::bind(std::forward<F>(func), std::forward<T>(params)...);

        using ReturnType = typename std::invoke_result<F, T...>::type;//因为类型表达式有模板类型F,T，所以要用typename显示修饰
        using PackagedTask = std::packaged_task<ReturnType()>;
        //task是指向PackagedTask类型的指针，这个指针指向的内存块就是bind绑定的函数，所以*task就是拿到函数了
        auto task = std::make_shared<PackagedTask>(std::move(execute));
        auto result = task->get_future(); //用于在任务完成时获取结果

        MutexGuard lock(threadpool_mutex);
        assert(!stop);

        tasks.emplace( [task]() {(*task)();});

        if (idleThreads > 0){
            cv.notify_one();
        }
        else if (currentThreads < maxThreads){
            Thread t(&ThreadPool::worker, this);
            assert(threads.find(t.get_id()) == threads.end());
            threads[t.get_id()] = std::move(t);
            ++currentThreads;
        }

        return result;
    }

    size_t ThreadsNum() const
    {
        MutexGuard lock(threadpool_mutex);
        return currentThreads;
    }

    void set_maxthreads(size_t maxthreadpool) {//只允许初始化一次
        if (maxThreads == 0) {
            maxThreads = maxthreadpool;
        }
    }
private:
    ThreadPool() : stop(false), currentThreads(0), idleThreads(0), maxThreads(0) {}

    ~ThreadPool() {
        {
            MutexGuard lock(threadpool_mutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& item : threads) {
            assert(item.second.joinable());
            item.second.join();
        }
    }

    void worker() {
        while (true) {
            try {
                Task task;
                {
                    UniqueLock uniqueLock(threadpool_mutex);
                    ++idleThreads;
                    // 等待条件（1.stop为真2.来任务了），等到了，返回true，就唤醒，没等到就是超时
                    auto hasTimedout = !cv.wait_for(uniqueLock,
                        std::chrono::seconds(WAIT_SECONDS),
                        [this]() {return stop || !tasks.empty(); });
                    --idleThreads;
                    if (tasks.empty()) {
                        if (stop) {
                            --currentThreads;
                            return;
                        }
                        if (hasTimedout) {
                            --currentThreads;
                            joinFinishedThreads();  // 回收其他已退出的线程
                            finishedThreadIDs.emplace(std::this_thread::get_id());  //把自己放在待回收的队列里，以便别人回收，接力回收机制
                            return;
                        }
                    }
                    // 关键修复：检查队首任务是否为空，跳过无效任务，避免后续执行空函数导致未定义行为
                    if (!tasks.front()) {
                        tasks.pop();
                        continue;
                    }
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                if (task) {
                    task();
                }
                else {
                    std::cerr << "Thread pool worker got empty task after move! Skipping..." << std::endl;
                    continue;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Thread worker exception: " << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "Thread worker unknown exception" << std::endl;
            }
        }
    }

    void joinFinishedThreads(){
        while (!finishedThreadIDs.empty()){
            auto id = std::move(finishedThreadIDs.front());
            finishedThreadIDs.pop();
            auto iter = threads.find(id);

            assert(iter != threads.end());
            assert(iter->second.joinable());

            iter->second.join();
            threads.erase(iter);
        }
    }

    static constexpr size_t               WAIT_SECONDS = 2;

    bool                                  stop;
    size_t                                currentThreads;
    size_t                                idleThreads;
    size_t                                maxThreads;

    std::condition_variable               cv;
    std::queue<Task>                      tasks;
    std::queue<ThreadID>                  finishedThreadIDs;
    std::unordered_map<ThreadID, Thread>  threads;

};
constexpr size_t ThreadPool::WAIT_SECONDS;

#endif