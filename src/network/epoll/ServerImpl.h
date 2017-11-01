#ifndef AFINA_NETWORK_EPOLL_SERVER_H
#define AFINA_NETWORK_EPOLL_SERVER_H

#include <atomic>
#include <pthread.h>
#include <vector>

#include <afina/network/Server.h>

namespace Afina {
    namespace Network {
        namespace Epoll {
/**
 * # Network resource manager implementation
 * Server that is spawning a separate thread for each connection
 */
            class ServerImpl : public Server {
            public:
                ServerImpl(std::shared_ptr<Afina::Storage> ps);
                ~ServerImpl();

                // See Server.h
                void Start(uint32_t port, uint16_t workers) override;

                // See Server.h
                void Stop() override;

                // See Server.h
                void Join() override;

            protected:
                /**
                 * Method is calling epoll_wait in a separate thread
                 */
                void RunEpoll();

            private:
                // Atomic flag to notify threads when it is time to stop. Note that
                // flag must be atomic in order to safely publish changes cross thread
                // bounds
                std::atomic<bool> running;

                // Thread accepting and crunching client connections
                std::vector<pthread_t> workers;

                // Port number to listen on
                uint16_t listen_port;
            };

        } // namespace Blocking
    } // namespace Network
} // namespace Afina

#endif // AFINA_NETWORK_BLOCKING_SERVER_H