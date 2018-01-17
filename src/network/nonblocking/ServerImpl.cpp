#include "ServerImpl.h"


namespace Afina {
namespace Network {
namespace NonBlocking {

const size_t num_events = 10;   
const int epoll_timeout = 5000; 
static const size_t buffer_size = 16;



void check_socket(struct sockaddr_in server_addr, int server_socket){
        if (server_socket == -1) {
            throw std::runtime_error("Failed to open socket");
        }

        int reuseaddr = 1, reuseport = 1;
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) == -1) {
            close(server_socket);
            throw std::runtime_error("Socket setsockopt() failed");
        }

        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport)) == -1) {
            close(server_socket);
            throw std::runtime_error("Socket setsockopt() failed");
        }

        if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
            close(server_socket);
            throw std::runtime_error("Socket bind() failed");
        }

        if (listen(server_socket, 5) == -1) {
            close(server_socket);
            throw std::runtime_error("Socket listen() failed");
        }

        if (setsocknonblocking(server_socket))
            throw std::runtime_error("Couldn't set O_NONBLOCK to server socket");

}




ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps) : Server(ps) {}

// See Server.h
ServerImpl::~ServerImpl() {}

// See Server.h
// See Server.h
void ServerImpl::Start(uint32_t port, uint16_t n_workers) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0)
        throw std::runtime_error("Unable to mask signals");
    if ((uint16_t)port == port)
        listen_port = (uint16_t)port;
    else
        throw std::overflow_error("port wouldn't fit in a 16-bit value");

    workers.resize(n_workers);
    running.store(true);
    for (uint16_t i = 0; i < n_workers; i++)
        if (pthread_create(&workers[i], NULL, Epoll_Proxy, this) < 0)
            throw std::runtime_error("Could not create epoll thread");
}

// See Server.h
void ServerImpl::Stop() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running.store(false);
}

// See Server.h
void ServerImpl::Join() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    void *retval;
    for (pthread_t &epoll_thread : workers) {
        if (pthread_join(epoll_thread, &retval))
            throw std::runtime_error("pthread_join failed");
        if (retval) 
            throw std::runtime_error("epoll thread had encountered an error");
    }
    workers.clear();
}


struct client_fd : ep_fd {
    int epoll_fd;
    std::shared_ptr<Afina::Storage> ps;
    std::vector<char> buf;
    size_t offset;
    std::string out;
    std::list<client_fd> &list;
    std::list<client_fd>::iterator self;
    bool bailout;
    Protocol::Parser parser;
    client_fd(int fd_, int epoll_fd_, std::shared_ptr<Afina::Storage> ps_, std::list<client_fd> &list_,
              std::list<client_fd>::iterator self_)
            : ep_fd(fd_), epoll_fd(epoll_fd_), ps(ps_), offset(0), list(list_), self(self_), bailout(false) {
        buf.resize(buffer_size);
    }
    void cleanup() {
        epoll_modify(epoll_fd, EPOLL_CTL_DEL, 0, *this);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        list.erase(self);

    }
    void advance(uint32_t events) override {
        if (events & (EPOLLHUP | EPOLLERR))
            return cleanup();

        ssize_t len = 0; 
        do {
            offset += len;
            if (buf.size() == offset)
                buf.resize(std::max(buf.size() * 2, buffer_size));
            len = recv(fd, buf.data() + offset, buf.size() - offset, 0);
        } while (len > 0);

        if (errno != EWOULDBLOCK && errno != EAGAIN)
            return cleanup();
        if (!len)
            bailout = true;


        if (!out.size()) {
            try {
                for (;;) {
                    uint32_t body_size;

                    auto cmd = parser.Build(body_size);
                    if (!cmd) { 
                        size_t parsed = 0;
                        parser.Parse(buf.data(), offset, parsed);
                        buf.erase(buf.begin(), buf.begin() + parsed);
                        offset -= parsed;
                        cmd = parser.Build(body_size);
                    }
                    if (cmd && (!body_size || offset >= body_size + 2)) {
                        
                        std::string body;
                        parser.Reset();
                        if (body_size) {

                            body.assign(buf.data(), body_size);
                            buf.erase(buf.begin(), buf.begin() + body_size + 2);
                            offset -= (body_size + 2);
                        }
                        std::string local_out;
                        cmd->Execute(*ps, body, local_out);
                        out += local_out;
                        out += std::string("\r\n");
                    } else {
                        
                        break;
                    }
                }
            } catch (std::runtime_error &e) {

                out += std::string("CLIENT_ERROR ") + e.what() + std::string("\r\n");
                bailout = true;
            }
        }


        if (out.size()) {
            do {
                len = send(fd, out.data(), out.size(), 0);
                if (len > 0)
                    out.erase(0, len);
            } while (out.size() && len > 0);
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                return cleanup();
        }

        if (bailout)
            return cleanup();
    }
};



struct listen_fd : ep_fd {
    int epoll_fd;
    std::shared_ptr<Afina::Storage> ps;
    std::list<client_fd> &client_list;
    listen_fd(int fd_, int epoll_fd_, std::shared_ptr<Afina::Storage> ps_, std::list<client_fd> &client_list_)
            : ep_fd(fd_), epoll_fd(epoll_fd_), ps(ps_), client_list(client_list_) {}
    void advance(uint32_t events) override {
        
        if (events & (EPOLLHUP | EPOLLERR)) {
            close(fd);
            throw std::runtime_error("Caught error state on listen socket");
        }

        struct sockaddr_in client_addr;
        socklen_t sinSize = sizeof(struct sockaddr_in);
        int client_socket;
        while ((client_socket = accept(fd, (struct sockaddr *)&client_addr, &sinSize)) !=
               -1) { 
            if (setsocknonblocking(client_socket))
                throw std::runtime_error("Couldn't set client socket to non-blocking");

            auto cl_it = client_list.emplace(client_list.end(), client_socket, epoll_fd, ps, client_list,
                                             client_list.end() /* see below */);
            cl_it->self = cl_it;


            //
            if (epoll_modify(epoll_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLOUT | EPOLLET, *cl_it))
                throw std::runtime_error("epollctl failed to add client socket");
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; 

        close(fd);
        throw std::runtime_error("Socket accept() failed");
    }
};




// See Server.h
void ServerImpl::RunEpoll() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;


    int epoll_sock = epoll_create1(0);

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;          
    server_addr.sin_port = htons(listen_port); 
    server_addr.sin_addr.s_addr = INADDR_ANY;  

    int server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);


    check_socket(server_addr, server_socket);



    std::list<client_fd> client_list; 
    listen_fd listening_object{server_socket, epoll_sock, pStorage, client_list}; 
    if (epoll_modify(epoll_sock, EPOLL_CTL_ADD, EPOLLIN | EPOLLET, listening_object))
        throw std::runtime_error("epoll_ctl failed to add the listen socket");



    while (running.load()) {
        epoll_event events[num_events];
        int events_now = epoll_wait(epoll_sock, events, num_events, epoll_timeout);
        if (events_now < 0) {
            if (errno == EINTR)
                continue; 
            else
                throw std::runtime_error("networking epoll returned error");
        }
        for (int i = 0; i < events_now; i++)
            ((ep_fd *)events[i].data.ptr)->advance(events[i].events);
    }


    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    for (client_fd &cl : client_list) {
        shutdown(cl.fd, SHUT_RDWR);
        close(cl.fd);
    }
    close(epoll_sock);
}

} // namespace Blocking
} // namespace Network
} // namespace Afina
