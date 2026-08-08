#pragma once
#include<unordered_map>
#include<string>
#include<vector>
#include<condition_variable>
#include<shared_mutex>
#include<memory>
#include<fstream>
#include<mutex>
#include<queue>
#include<thread>

struct Shard{
    std::unordered_map<std::string,std::string> mp;
    std::shared_mutex mut;
};

class HashTable{
    std::vector<std::unique_ptr<Shard>> shards;
    int NUM_SHARDS;

    std::string fileName;
    std::ofstream outputFile;

    std::mutex wal_mut;
    std::condition_variable wal_cv;
    std::queue<std::string> wal_queue;

    bool stop_wal;
    std::thread wal_thread;

    void WAL_worker();
    int getShard(const std::string &key);

    public:

    HashTable(const std::string &fileName,int num_shards);

    void set(const std::string &key,const std::string &val);
    std::string get(const std::string &key);
    void del(const std::string &key);

    ~HashTable();
};