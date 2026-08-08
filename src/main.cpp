#include<iostream>
#include"HashTable.hpp"
#include"Task.hpp"
#include"ThreadPool.hpp"
#include<string>

int main()
{
    HashTable tb("cache.log",16);
    ThreadPool pool(8);

    std::string command;
    std::string k, v;

    while (std::cin >> command)
    {
        if (command == "GET")
        {
            std::cin >> k;

            pool.enqueue(new getTask(tb,k));
        }

        else if (command == "SET")
        {
            std::cin >> k >> v;

            pool.enqueue(new setTask(tb,k,v));
        }

        else if (command == "DEL")
        {
            std::cin >> k;

            pool.enqueue(new delTask(tb,k));
        }

        else if (command == "exit")
            break;

        else
        {      
            // discarding garbage input
            std::cerr << "Invalid Command\n";
        }

        std::string dummy;
        getline(std::cin,dummy);
    }

    std::cout << "Exiting Program\n";

    return 0;
}