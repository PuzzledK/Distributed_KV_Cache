#include "Task.hpp"

std::string setTask :: execute (){
    tb.set(key,val);
    return {};
}

std::string getTask :: execute (){
    return tb.get(key);
}

std::string delTask :: execute(){
    tb.del(key);
    return {};
}