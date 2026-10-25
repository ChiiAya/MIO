#pragma once

#include <memory>
#include <string>

namespace ix {
class HttpServer;
}

namespace mio {

class Runtime;

class AdminServer {
public:
    AdminServer(Runtime& runtime, int port = 6188, std::string host = "127.0.0.1");
    ~AdminServer();

    bool start();
    void stop();

    int port() const { return port_; }
    const std::string& host() const { return host_; }
    bool isRunning() const { return running_; }

private:
    Runtime& runtime_;
    int port_;
    std::string host_;
    bool running_ = false;
    std::unique_ptr<ix::HttpServer> server_;
};

} // namespace mio

