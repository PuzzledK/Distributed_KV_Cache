#pragma once
#include "Task.hpp"

class ThreadPool{
    std::vector<std::thread> threads;

    std::queue<Task*> taskQueue;
    std::mutex queueMutex;
    bool stop;
    std::condition_variable taskCV;

    void Worker();

    public:

    ThreadPool(int n);

    void enqueue(Task* task);

    ~ThreadPool();
};