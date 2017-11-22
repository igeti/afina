#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <pthread.h> // pthread_sigmask
#include <signal.h>  // sigset_t
#include <stdlib.h>  // realpath
#include <sys/stat.h>  // umask
#include <sys/types.h> // getpid
#include <unistd.h>

#include <cxxopts.hpp>

#include <afina/Storage.h>
#include <afina/Version.h>
#include <afina/network/Server.h>


#include "network/blocking/ServerImpl.h"
#include "network/epoll/ServerImpl.h"
#include "network/uv/ServerImpl.h"
#include "storage/MapBasedGlobalLockImpl.h"

typedef struct {
    std::shared_ptr<Afina::Storage> storage;
    std::shared_ptr<Afina::Network::Server> server;
} Application;

// Handle all signals catched
void signal_handler(uv_signal_t *handle, int signum) {
    Application *pApp = static_cast<Application *>(handle->data);

    std::cout << "Receive stop signal" << std::endl;
    uv_stop(handle->loop);
}

// Called when it is time to collect passive metrics from services
void timer_handler(uv_timer_t *handle) {
    Application *pApp = static_cast<Application *>(handle->data);
    std::cout << "Start passive metrics collection" << std::endl;
}

int main(int argc, char **argv) {
    // Build version
    // TODO: move into Version.h as a function
    std::stringstream app_string;
    app_string << "Afina " << Afina::Version_Major << "." << Afina::Version_Minor << "." << Afina::Version_Patch;
    if (Afina::Version_SHA.size() > 0) {
        app_string << "-" << Afina::Version_SHA;
    }

    // Command line arguments parsing
    cxxopts::Options options("afina", "Simple memory caching server");
    try {
        // TODO: use custom cxxopts::value to print options possible values in help message
        // and simplify validation below
        options.add_options()("s,storage", "Type of storage service to use", cxxopts::value<std::string>());
        options.add_options()("n,network", "Type of network service to use", cxxopts::value<std::string>());
        options.add_options()("d,daemonize", "Daemonize to background"); // boolean by default
        options.add_options()("p,pidfile", "Path of pidfile", cxxopts::value<std::string>());
        options.add_options()("h,help", "Print usage info");
        options.parse(argc, argv);

        if (options.count("help") > 0) {
            std::cerr << options.help() << std::endl;
            return 0;
        }
    } catch (cxxopts::OptionParseException &ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        return 1;
    }

    // Start boot sequence
    Application app;
    std::cout << "Starting " << app_string.str() << std::endl;

    // Build new storage instance
    std::string storage_type = "map_global";
    if (options.count("storage") > 0) {
        storage_type = options["storage"].as<std::string>();
    }

    if (storage_type == "map_global") {
        app.storage = std::make_shared<Afina::Backend::MapBasedGlobalLockImpl>();
    } else {
        throw std::runtime_error("Unknown storage type");
    }

    // Build  & start network layer
    std::string network_type = "blocking";
    if (options.count("network") > 0) {
        network_type = options["network"].as<std::string>();
    }

    if (network_type == "uv") {
        app.server = std::make_shared<Afina::Network::UV::ServerImpl>(app.storage);
    } else if (network_type == "blocking") {
        app.server = std::make_shared<Afina::Network::Blocking::ServerImpl>(app.storage);
    }
    else if (network_type == "nonblocking") {
            app.server = std::make_shared<Afina::Network::Epoll::ServerImpl>(app.storage);
    }
    else {
        throw std::runtime_error("Unknown network type");
    }

    // Init local loop. It will react to signals and performs some metrics collections. Each
    // subsystem is able to push metrics actively, but some metrics could be collected only
    // by polling, so loop here will does that work
    {
        std::ofstream pfs;
        if (options.count("pidfile")) {
            std::string pidfile = options["pidfile"].as<std::string>();
            // real path storage
            std::unique_ptr<char, decltype(&std::free)> rpath{nullptr, &std::free};
            // create the pidfile and prepare to write
            pfs.open(pidfile.c_str());
            // store the *real* path because we may chdir later
            rpath.reset(realpath(pidfile.c_str(), nullptr));
            if (!rpath)
                throw std::runtime_error("Failed to get real path of PID file");

            pidfile.assign(rpath.get());
        }
        if (options.count("daemonize")) {
            // 1. fork() off to
            //  a. return the control to the shell
            //  b. *not* to remain a process group leader
            pid_t pid = fork();
            if (pid < 0) {
                std::cerr << "Failed to fork()" << std::endl;
                return 1;
            } else if (pid > 0) // parent should exit
                return 0;

            // 2. setsid() to become a process+session group leader
            //  (we won't have a controlling terminal this way)
            if (setsid() == -1)
                return 1; // Probably shouldn't print anything at this point

            // 3. fork() off again, leaving the session group without a leader
            //  (this makes sure we stay without a controlling terminal forever)
            if ((pid = fork()) < 0)
                return 1;
            else if (pid > 0)
                return 0;

            // 4. Don't occupy directory handles if we can help it
            chdir("/");
            // 5. Whatever umask we inherited, drop it
            umask(0);

            // 6. (cin/cout/cerr are tied to stdin/stdout/stderr)
            freopen("/dev/null", "r", stdin);
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
        }
        if (pfs) { // we were asked to create a pidfile - now is time to fill it
            pfs << getpid() << std::endl;
        }
    }





    uv_loop_t loop;
    uv_loop_init(&loop);

    uv_signal_t sig;
    uv_signal_init(&loop, &sig);
    uv_signal_start(&sig, signal_handler, SIGTERM | SIGKILL);
    sig.data = &app;

    uv_timer_t timer;
    uv_timer_init(&loop, &timer);
    timer.data = &app;
    uv_timer_start(&timer, timer_handler, 0, 5000);

    // Start services
    try {
        app.storage->Start();
        app.server->Start(8080);

        // Freeze current thread and process events
        std::cout << "Application started" << std::endl;
        uv_run(&loop, UV_RUN_DEFAULT);

        // Stop services
        app.server->Stop();
        app.server->Join();
        app.storage->Stop();

        std::cout << "Application stopped" << std::endl;
    } catch (std::exception &e) {
        std::cerr << "Fatal error" << e.what() << std::endl;
    }

    return 0;
}
