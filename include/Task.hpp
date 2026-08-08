#pragma once
#include<string>
#include "HashTable.hpp"

class Task{
    public:
    
    virtual std::string execute() = 0;
    virtual ~Task() {};
};

class setTask : public Task{
    HashTable &tb;
    std::string key;
    std::string val;

    public:
    setTask(HashTable &tb,const std::string &key,const std::string &val)
    : tb(tb),key(key),val(val){};
    
    std::string execute();
};

class getTask : public Task{
    HashTable &tb;
    std::string key;

    public:
    getTask(HashTable &tb,const std::string &key)
    : tb(tb),key(key){};

    std::string execute();
};

class delTask : public Task{
    HashTable &tb;
    std::string key;

    public:
    delTask(HashTable &tb,const std::string &key)
    : tb(tb),key(key){};

    std::string execute();
};