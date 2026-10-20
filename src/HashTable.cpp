#include "HashTable.hpp"

void HashTable :: WAL_worker(){
    while(true){
        std::string log;
        {
            std::unique_lock<std::mutex> lock(wal_mut);
            wal_cv.wait(lock,[this]{
                return stop_wal || !wal_queue.empty();
            });

            if (stop_wal && wal_queue.empty()) return;

            log = wal_queue.front();
            wal_queue.pop();
            
        }

        outputFile << log << std::endl;
        outputFile.flush();
    }    
}

int HashTable :: getShard(const std::string &key){
    return std::hash<std::string>{}(key) % NUM_SHARDS;
}


HashTable :: HashTable(const std::string &logFileName,int num_shards)
    : NUM_SHARDS(num_shards), fileName(logFileName), stop_wal(false){
    for(int i = 0;i < num_shards;i++){
        shards.push_back(std::make_unique<Shard>());
    }

    std::fstream file(logFileName,std::ios::in);
    if(file.is_open()){
        std::string cmd,k,v;

        while(file>>cmd){
            if(cmd == "SET"){
                file>>k>>v;

                int shardNum = getShard(k);
                shards[shardNum] -> mp[k] = v;
            }
            else if(cmd == "DEL"){
                file>>k;

                int shardNum = getShard(k);
                shards[shardNum] -> mp.erase(k);
            }
            else{
                std::string dummy;
                std::getline(file,dummy);
            }
        }
    }

    file.close();
    outputFile.open(logFileName,std::ios::app);
    
    if(!outputFile.is_open()){
        throw std::runtime_error("Failed to open Log file");
    }

    wal_thread = std::thread(&HashTable :: WAL_worker,this);
}

void HashTable :: set(const std::string &key,const std::string &val){
    {
        std::unique_lock<std::mutex> lock(wal_mut);
        wal_queue.push("SET "+ key + " " + val);
    }

    wal_cv.notify_one();
    int shardNum = getShard(key);
    
    {
        std::unique_lock<std::shared_mutex> lock(shards[shardNum] -> mut);
        shards[shardNum] -> mp[key] = val;
    }
}

std::string HashTable :: get(const std::string &key){
    std::string value;
    if (!get(key, value)) {
        return "Key Not Found";
    }
    return value;
}

bool HashTable :: get(const std::string &key,std::string &value){
    int shardNum = getShard(key);
    
    {
        std::shared_lock<std::shared_mutex> lock(shards[shardNum] -> mut);
        
        auto it = shards[shardNum] -> mp.find(key);
        if(it == shards[shardNum] -> mp.end()){
            return false;
        }

        value = it -> second;
        return true;
    }
}

void HashTable :: del(const std::string &key){
    {
        std::unique_lock<std::mutex> lock(wal_mut);
        wal_queue.push("DEL "+ key);
    }

    wal_cv.notify_one();
    int shardNum = getShard(key);
    
    {
        std::unique_lock<std::shared_mutex> lock(shards[shardNum] -> mut);
        shards[shardNum] -> mp.erase(key);
    }
}

HashTable :: ~HashTable(){
    {
        std::unique_lock<std::mutex> lock(wal_mut);
        stop_wal = true;
    }
    wal_cv.notify_one();

    if(wal_thread.joinable()) wal_thread.join();
    if(outputFile.is_open()) outputFile.close();
}