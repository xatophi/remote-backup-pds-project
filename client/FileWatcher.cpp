//
// Created by giacomo on 31/07/20.
//

#include <thread>
#include <iostream>
#include <deque>
#include <condition_variable>

#include "FileWatcher.h"
#include "client.h"
#include "ExceptionBackup.h"

std::mutex om;

void myout(const std::string& msg){
    std::lock_guard og(om);
    std::clog<<msg<<std::endl;
}


template <class T>
class Jobs {
    // FIFO queue
    std::deque<T> jobs;
    bool active = true;
    std::mutex qm;
    std::condition_variable not_empty;

public:
    void put(T job){
        std::lock_guard _g(qm);
        jobs.push_back(job);
        not_empty.notify_one();
    }

    std::optional<T> get() {
        std::unique_lock lock(qm);
        while (true){

            if(!jobs.empty()){
                T job = jobs.front();
                jobs.pop_front();
                return job;
            }
            if(!active){
                return std::optional<T>();
            }
            not_empty.wait(lock, [this](){ return !this->jobs.empty() || !this->active;});
        }
    }

    void ended(){
        std::lock_guard g(qm);
        active = false;
        not_empty.notify_all();
    }

//    int size(){
//        return jobs.size();
//    }
};

FileWatcher::FileWatcher(const std::string& path_to_watch, std::chrono::duration<int, std::milli> delay)
    : path_to_watch{path_to_watch}, delay{delay} {

    try {
        initialization();
    }
    catch (const ExceptionBackup& e) {
        // server error or connection lost

        std::stringstream ss;
        ss << e.what() <<". Error number " << e.getErrorNumber()  << std::endl;
        myout(ss.str());
        if(check_connection_and_retry())
            myout("Connection is back");
    }
}

void FileWatcher::initialization() {
    // object implementing a fifo queue of directories and sub-directories present in the path to watch
    Jobs<std::filesystem::path> jobs;

    // number of threads equal to the number of cores of the machine
    unsigned int n_threads = std::thread::hardware_concurrency();

    std::vector<std::thread> threads_;
    threads_.resize(n_threads);

    // counter of leaf directories in the queue (when the counter comes back to 0, it means that all the tree of directories has been watched)
    std::atomic<int> count_leaves(0);

    // delete any files or folders no longer present in the root folder
    // probe_folder(path_to_watch);

    // erase all the elements in the path_ map
    paths_.clear();

    // firstly insert in the queue the root directory
    jobs.put(path_to_watch);
    // increment the counter of leaf directories present in the fifo queue
    count_leaves.fetch_add(1);


    for(int i=0; i<n_threads; i++){
        threads_.emplace_back([&jobs, this, &count_leaves, i](){
            while(true){
                // take a job from the queue
                std::optional<fs::path>  path_ = jobs.get();
                if(path_.has_value()){
                    // safe because only a thread will receive that path
                    fs::path path_entry = path_.value();

                    // counter of direct child-directories of the current directory
                    int directories = 0;

                    // myout("sending probe folder of " + path_entry.string());
                    if(!probe_folder(path_entry.string())) {
                        // myout("probe failed, sending backup folder " + path_entry.string());
                        backup_folder(path_entry.string());
                    }
                    // add the folder to the paths_ unordered_map or just update last write time if already present
                    mutex_paths_.lock();
                    paths_[path_entry.string()] = fs::last_write_time(path_entry);
                    mutex_paths_.unlock();

                    // iterate on all direct children of the directory
                    for (const auto& p : fs::directory_iterator(path_entry)) {
                        if (p.is_regular_file()) {
                            // myout("sending probe file of " + p.path().string());
                            if (!probe_file(p.path().string())) {
                                // myout("probe failed, sending backup file " + p.path().string());
                                backup_file(p.path().string());
                            }
                            // add the file to the paths_ unordered_map or just update last write time
                            // if already present
                            mutex_paths_.lock();
                            paths_[p.path().string()] = fs::last_write_time(p);
                            mutex_paths_.unlock();
                        } else if (p.is_directory()) {
                            directories++;
                            if (directories >= 2) {
                                // increment count_leaves only if more then one sub-directory
                                count_leaves.fetch_add(1);
                            }
                            jobs.put(p.path());
                        }
                    }

                    // if there are no sub-directories, the actual directory is really a leaf, decrement counter
                    // because finished to watch it
                    if (directories == 0)
                        count_leaves.fetch_sub(1);

                    // if counter of leaf directories is 0 means that all the tree has been visited, so stop
                    // the jobs queue
                    if (count_leaves.load() == 0){
                        jobs.ended();
                    }
                } else {
                    // myout("consumer " + std::to_string(i) + " terminated");
                    break;
                }
            }
        });
    }

    for (auto &t: threads_) {
        if(t.joinable()) t.join();
    }

    // myout("consumer terminati job ancora aperti: " + std::to_string(jobs.size()));

}

bool FileWatcher::check_connection_and_retry() {
    while(retry) {
        // sleep 30 seconds (+ potentially another 60 seconds of connection timeout)
        std::this_thread::sleep_for(std::chrono::seconds(30));

        try {
            // the server connection is active
            initialization();
            return true;
        }
        catch (const ExceptionBackup& e) {
            // server error or connection lost
            std::cerr << e.what() << ". Error number " << e.getErrorNumber() << std::endl;
            if(e.getErrorType() == http_error)
                retry--;
        }
    }
    return false;
}

void FileWatcher::start() {
    while (retry) {
        try {
            // Wait for "delay" milliseconds
            std::this_thread::sleep_for(delay);

            auto it = paths_.begin();
            while (it != paths_.end()) {
                // file / folder elimination
                if (!fs::exists(it->first)) {
                    // myout("delete path of " + it->first);
                    // delete from server
                    delete_path(it->first);

                    // update last modified time of it's parent folder in paths_
                    int pos = it->first.rfind("/");
                    std::string parent= it->first.substr(0, pos);
                    if (fs::exists(parent))
                        paths_[parent] = fs::last_write_time(parent);

                    // delete from paths
                    it = paths_.erase(it);
                } else {
                    it++;
                }
            }

            for (auto &path_entry : fs::recursive_directory_iterator(path_to_watch)) {
                auto current_file_last_write_time = fs::last_write_time(path_entry);

                // file / folder creation
                if (!contains(path_entry.path().string())) {

                    if(path_entry.is_directory()) {
                        // myout("backup folder " + path_entry.path().string());
                        backup_folder(path_entry.path().string());
                    }
                    else if(path_entry.is_regular_file()) {
                        // myout("backup file " + path_entry.path().string());
                        backup_file(path_entry.path().string());
                    }
                    paths_[path_entry.path().string()] = current_file_last_write_time;

                } else {
                    // file  modification
                    if (paths_[path_entry.path().string()] != current_file_last_write_time) {
                        // folder modification has not meaning since its renaming is considered as folder cancellation
                        // and then new creation. Modification of his children has not to be taken into account here but
                        // directly from them
                        /*if(path_entry.is_directory()) {
                            backup_folder(path_entry.path().string());
                        }*/
                        if(path_entry.is_regular_file()) {
                            // myout("file modified: sending delete and backup " + path_entry.path().string());
                            delete_path(path_entry.path().string());
                            backup_file(path_entry.path().string());
                        }
                        paths_[path_entry.path().string()] = current_file_last_write_time;
                    }
                }
            }
        }
        catch (const ExceptionBackup& e) {
            // server error or connection lost
            std::cerr << e.what() << ". Error number " << e.getErrorNumber() << std::endl;
            if(check_connection_and_retry())
                myout("Connection is back");
        }
    }
}
