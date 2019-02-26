#include <iostream>

#include "../WSServer.hpp"

class EvoServer : public agriic::WSServer
{
public:
    EvoServer(int port) : agriic::WSServer(port)
    {}

protected:

    void onWsConnect(void *id) override
    {
        std::cout << "new client\n";
    }

    void onWsDisconnect(void *id) override
    {
        std::cout << "disconnect\n";
    }

    void onWsMessage(void *id, const std::string& data) override
    {
        std::cout << "Message: " << data << "\n";

        send(id, "Hello");
    }
};

int main()
{
    EvoServer ws(3221);

    ws.handleGet("/kuku", [](std::string url) {
        return "Hello";
    });

    ws.start();

    // run loop

    // ws.run(); or
    while (true) { // while not stopped
        ws.wait();
    }
}
