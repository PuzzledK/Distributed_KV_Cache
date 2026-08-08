#include "ThreadPool.hpp"
#include <exception>

void ThreadPool :: Worker()
{
    while (true)
    {

        Task *task = nullptr;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            taskCV.wait(lock, [this]
                        { return stop || !taskQueue.empty(); });

            if (stop && taskQueue.empty())
                return;

            task = taskQueue.front();
            taskQueue.pop();
        }

        try
        {
            task->execute();
        }
        catch (const std::exception &e)
        {
            delete task;
            throw std::runtime_error(e.what());
        }

        delete task;
    }
}

ThreadPool :: ThreadPool(int n){
    for(int i = 0;i < n;i++){
        threads.emplace_back(std::thread(&ThreadPool::Worker,this));
    }
}

void ThreadPool :: enqueue(Task *task){
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        taskQueue.push(task);
    }
    taskCV.notify_one();
}

ThreadPool :: ~ThreadPool(){
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }

    taskCV.notify_all();

    for(auto &i : threads){
        if(i.joinable()) i.join();
    }
}