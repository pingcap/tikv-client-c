#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>

#include <unistd.h>

namespace
{

using namespace Poco::Net;

class Client
{
public:
    Client()
        : manager_addr(std::getenv("MANAGER_ADDR")), box_id(std::stoi(std::getenv("BOX_ID"))), self_id(std::stoi(std::getenv("TEST_ID")))
    {
        std::cerr << "manager addr: " << manager_addr << std::endl;
        std::cerr << "box id: " << box_id << std::endl;
        std::cerr << "self id: " << self_id << std::endl;
        getConfig();
    }

    std::string debug_string(std::string str)
    {
        for (size_t i = 0; i < str.size(); i++)
        {
            if (str[i] == '\n')
                str[i] = ' ';
        }
        return str;
    }

    void getConfig()
    {
        std::string manager_addr_str(manager_addr);
        int begin = manager_addr_str.find("://") + 3;
        std::string rest_info = manager_addr_str.substr(begin);
        std::string host = rest_info.substr(0, rest_info.find(":"));
        int port = std::stoi(rest_info.substr(rest_info.find(":") + 1));
        std::string url = manager_addr + std::string("/testConfig/") + std::to_string(self_id);
        SocketAddress addr(host, port);
        HTTPClientSession sess(addr);
        HTTPRequest req(HTTPRequest::HTTP_GET, url);
        sess.sendRequest(req);
        HTTPResponse res;
        auto & is = sess.receiveResponse(res);
        char buffer[1200];
        std::string json_str;
        for (;;)
        {
            is.read(buffer, 256);
            if (is)
            {
                json_str.append(buffer, 256);
            }
            else
            {
                json_str.append(buffer, is.gcount());
                break;
            }
        }
        config = json_str;
    }

    std::vector<std::string> PDs()
    {
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var result = parser.parse(config);
        auto json_obj = result.extract<Poco::JSON::Object::Ptr>();
        auto data = json_obj->getObject("data");
        auto cat = data->getObject("cat");
        auto pds = cat->getArray("pds");
        std::vector<std::string> rets;
        for (size_t i = 0; i < pds->size(); i++)
        {
            auto ip = pds->getObject(i)->getValue<std::string>("ip");
            auto port = pds->getObject(i)->getValue<int>("service_port");
            std::string addr = ip + ":" + std::to_string(port);
            std::cerr << "pd addr: " << addr << std::endl;
            rets.push_back(addr);
        }
        return rets;
    }

    const char * manager_addr;
    int box_id;
    int self_id;
    std::string config;
};
} // namespace
