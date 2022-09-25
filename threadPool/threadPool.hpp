#include <queue>
#include <mutex>
#include <future>
#include <vector>
#include <functional>
#include <thread>
#include <utility>

//一个加了锁，线程安全的queue
template <typename T>
class SafeQueue
{
private:
    std::queue<T> que;
    std::mutex mtx;
public:
    SafeQueue() = default;

    bool empty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return que.empty();
    }

    int size()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return que.size();
    }

    void push(const T& t)
    {
        std::lock_guard<std::mutex> lock(mtx);
        que.emplace(t);
    }

    bool pop(T& t)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (que.empty())
        {
            return false;
        }
        t = std::move(que.front());
        que.pop();
        return true;
    }
};


class ThreadPool
{
private:
    //task 队列
    SafeQueue<std::function<void()>> taskQueue;
    //条件变量
    std::condition_variable conditionLock;
    //thread 队列
    std::vector<std::thread> threadVec;
    //mutex锁
    std::mutex mtx;
    //threadPool 状态
    bool isShutdown{false};
    //线程实际工作流程
    void threadWorker(int threadIndex)
    {
        std::function<void()> func;
        //当线程池状态不为关闭的情况下循环执行
        while (not isShutdown)
        {
            //为多线程操作加锁
            std::unique_lock<std::mutex> lock(mtx);
            //如果任务队列为空，则阻塞线程，等待notify
            if (taskQueue.empty())
            {
                conditionLock.wait(lock);
            }
            //从task队列中获取task
            auto result = taskQueue.pop(func);
            //解锁，其他线程可以去获取task
            lock.unlock();
            //执行具体task
            if (result)
            {
                func();
            }
        }
        //如果线程池状态为关闭，线程结束
    }

public:
    explicit ThreadPool(int threadNum): threadVec(std::vector<std::thread>(threadNum)) {};
    ThreadPool() = delete;
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool(const ThreadPool&&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&&) = delete;
    
    //初始化线程池
    void init()
    {
        int threadNum = threadVec.size();
        for (int i = 0; i < threadNum; ++i)
        {
            //用线程实际工作函数threadWorker初始化线程
            threadVec.at(i) = std::thread(&ThreadPool::threadWorker, this, i);
        }
    }

    template <typename F, typename... Args>
    //尾返回类型推导。submit的返回类型是是std::future<type>,其中type是传入函数f的返回类型
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
    {
        //使用std::bind 把有参数的函数包装成无参数的函数，方便后面调用（直接执行func())。std::forward进行完美转发
        std::function<decltype(f(args...))()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        //两层包装，最外层是一个智能指针，方便对象的管理。里层是将function包装成packaged_task,这样是为了方便进行异步操作和获取返回值future
        auto taskPtr = std::make_shared<std::packaged_task<decltype(f(args...))()>>(func);
        //将packaged_task包装成一个void function，方便调用
        std::function<void()> warpperFunc = [taskPtr](){
            (*taskPtr)();
        };
        //将任务放入任务队列中
        taskQueue.push(warpperFunc);
        //notify一个线程去执行
        conditionLock.notify_one();
        //异步返回task的执行结果
        return taskPtr->get_future();
    }

    void shutdown()
    {
        //更改线程状态
        isShutdown = true;
        //notify all 所有thread, 解除阻塞状态，往下执行，直到结束
        conditionLock.notify_all();
        int threadNum = threadVec.size();
        //逐一等待thread 结束
        for (int i = 0; i < threadNum; ++i)
        {
            if (threadVec.at(i).joinable())
            {
                threadVec.at(i).join();
            }
        }
    }
};