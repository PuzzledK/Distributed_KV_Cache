#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>

using namespace std;

mutex print_mutex;

struct shard{
    unordered_map<string,string> mp;
    shared_mutex mut;
};

class HashTable
{
    vector<unique_ptr<shard>> shards;
    int NUM_SHARDS;

    // unordered_map<string, string> mp;
    string filename;
    ofstream outputFile;
    // mutable shared_mutex map_mutex;

    mutex wal_mut;
    condition_variable wal_cv;
    queue<string> wal_queue;
    bool stop_wal;
    thread wal_thread;

    size_t getShard(const string& key) const
    {
        return hash<string>{}(key) % NUM_SHARDS;
    }

    void WAL_worker(){
        while(true){
            string log;

            {
                unique_lock<mutex> lock(wal_mut);
                wal_cv.wait(lock,[this]{
                    return stop_wal || !wal_queue.empty();
                });

                if(stop_wal && wal_queue.empty()) return;

                log = wal_queue.front();
                wal_queue.pop();
            }

            outputFile << log << endl;
        }
    }

public:
    HashTable(const string &filename,int num_shards) : stop_wal(false),NUM_SHARDS(num_shards)
    {
        this->filename = filename;

        ifstream file(filename);
        
        for(int i = 0;i < num_shards;i++){
            shards.push_back(make_unique<shard>());
        }

        if (file.is_open())
        {
            string op, k, v;

            while (file >> op)
            {
                if (op == "SET")
                {
                    file >> k >> v;

                    int shard_num = hash<string>{}(k) % NUM_SHARDS;

                    shards[shard_num]->mp[k] = v;
                }

                else if (op == "DEL")
                {
                    file >> k;

                    int shard_num = hash<string>{}(k) % NUM_SHARDS;

                    shards[shard_num]->mp.erase(k);
                }

                else
                {
                    // skipping useless line
                    string dummy;
                    getline(file, dummy);
                }
            }
        }

        file.close();

        outputFile.open(filename,ios::app);
        if(!outputFile.is_open())
        {
            throw runtime_error("Error in opening log file");
        }

        wal_thread = thread(&HashTable::WAL_worker,this);
    }

    ~HashTable(){
        {
            unique_lock<mutex> lock(wal_mut);
            stop_wal = true;
        }
        wal_cv.notify_one();

        if(wal_thread.joinable()) wal_thread.join();

        if(outputFile.is_open()) outputFile.close();
    }

    void set(const string &key, const string &val)
    {

        int shard_num = hash<string>{} (key) % NUM_SHARDS; 

        {
            unique_lock<mutex> lock(wal_mut);
            wal_queue.push("SET " + key + " " + val);
        }
        wal_cv.notify_one();

        {
            unique_lock<shared_mutex> lock(shards[shard_num]->mut);
            shards[shard_num]->mp[key] = val;
        }
    }

    string get(const string &key)
    {
        int shard_num = hash<string>{}(key) % NUM_SHARDS;
        {
        shared_lock<shared_mutex> lock(shards[shard_num]->mut);

        auto it = shards[shard_num]->mp.find(key);

        if (it == shards[shard_num]->mp.end())
            return "Key Not Found";

        return it->second;
        }
    }

    void del(const string &key)
    {
        int shard_num = hash<string>{}(key) % NUM_SHARDS;

        {
            unique_lock<mutex> lock(wal_mut);

            wal_queue.push("DEL " + key);
        }
        wal_cv.notify_one();

        {
            unique_lock<shared_mutex> lock(shards[shard_num]->mut);

            shards[shard_num]->mp.erase(key);
        }
    }
};

class Task
{
public:
    virtual void execute() = 0;
    virtual ~Task() {};
};

class setTask : public Task
{
    HashTable *tb;
    string key;
    string value;

public:
    setTask(HashTable *tb, const string &key, const string &value) : tb(tb), key(key), value(value) {}

    void execute() override
    {
        tb->set(key, value);
    }
};

class getTask : public Task
{
    HashTable *tb;
    string key;

public:
    getTask(HashTable *tb, const string &key) : tb(tb), key(key) {}

    void execute() override
    {   
        unique_lock<mutex> lock(print_mutex);
        cout<<tb->get(key)<<endl;
    }
};

class delTask : public Task
{
    HashTable *tb;
    string key;

public:
    delTask(HashTable *tb, const string &key) : tb(tb), key(key) {}

    void execute() override
    {
        tb->del(key);
    }
};

class batchTask : public Task{
    vector<Task*> tasks;

    public:
        batchTask(vector<Task*> &tasks) : tasks(tasks){}

        void execute() override{
            for(auto &task : tasks){
                task -> execute();
                delete task;
            }
        }
};

class ThreadPool
{
    vector<thread> workers;
    queue<Task *> tasks;
    condition_variable cv;
    mutex queue_mutex;
    bool stop;
    void workerFunction()
    {
        while (true)
        {
            Task *task = nullptr;

            {
                unique_lock<mutex> lock(queue_mutex);
                cv.wait(lock, [this]
                        { return stop || !tasks.empty(); });

                if (stop && tasks.empty())
                    return;

                task = tasks.front();
                tasks.pop();
            }

            try{
                task->execute();
            }
            catch(const exception& e){
                unique_lock<mutex> lock(print_mutex);

                cerr << e.what() <<endl;
            }
            
            delete task;
        }
    }

    public:
        ThreadPool(int numThreads) : stop(false)
        {
            for (int i = 0; i < numThreads; i++)
            {
                workers.emplace_back(&ThreadPool::workerFunction, this);
            }
        }

        void enqueue(Task* task){
            {
                unique_lock<mutex> lock(queue_mutex);
                tasks.push(task);
            }

            cv.notify_one();
        }

        ~ThreadPool(){
            {
                unique_lock<mutex> lock(queue_mutex);
                stop = true;
            }

            cv.notify_all();

            for(auto &i:workers){
                if(i.joinable()) i.join();
            }
        }
};

class Dispatcher{
    ThreadPool &pool;
    vector<Task*> task_batch;

    const int MAX_BATCH_SIZE = 1000;
    const int MAX_LINGER_MS = 5;
    bool stop;

    mutex batch_mutex;
    condition_variable cv;
    thread worker_thread;

    void worker(){
        while(true){
            vector<Task*> curr_batch;

            {
                unique_lock<mutex> lock(batch_mutex);
                cv.wait_for(lock,chrono :: milliseconds(MAX_LINGER_MS),[this]{
                    return stop || (task_batch.size() >= MAX_BATCH_SIZE);
                });

                if(stop && task_batch.empty()) return;

                if(!task_batch.empty()){
                    curr_batch = move(task_batch);
                    task_batch.clear();
                }
            }

            if(!curr_batch.empty()){
                pool.enqueue(new batchTask(curr_batch));
            }
        }
    }

    public:
        Dispatcher(ThreadPool &pool) : pool(pool), stop(false){
            worker_thread = thread(&Dispatcher::worker,this);
        }

        ~Dispatcher(){

            {
                unique_lock<mutex> lock(batch_mutex);
                stop = true;
            }
        
            cv.notify_one();

            if(worker_thread.joinable()) worker_thread.join();
        }

        void submit(Task* task){
            {
                unique_lock<mutex> lock(batch_mutex);
                task_batch.push_back(task);

                if(task_batch.size() >= MAX_BATCH_SIZE) cv.notify_one();
            }
        }

};

int main()
{
    HashTable tb("cache.log",16);
    ThreadPool pool(8);
    Dispatcher dispatcher(pool);

    string command;
    string k, v;

    while (cin >> command)
    {
        if (command == "GET")
        {
            cin >> k;

            dispatcher.submit(new getTask(&tb,k));
        }

        else if (command == "SET")
        {
            cin >> k >> v;

            dispatcher.submit(new setTask(&tb,k,v));
        }

        else if (command == "DEL")
        {
            cin >> k;

            dispatcher.submit(new delTask(&tb,k));
        }

        else if (command == "exit")
            break;

        else
        {      
            // discarding garbage input
            cerr << "Invalid Command\n";
        }

        string dummy;
        getline(cin,dummy);
    }

    std::cout << "Exiting Program\n";

    return 0;
}