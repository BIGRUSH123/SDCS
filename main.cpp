#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <cstdlib>
#include <map>
#include "httplib.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;

// 配置常量
namespace Config {
    constexpr int VIRTUAL_NODES = 150;
    constexpr int RPC_TIMEOUT_SECONDS = 5;
    constexpr int PORT_BASE = 9526;
}

class ConsistentHash {
private:
    map<uint32_t, string> ring;
    vector<string> nodes;
    int virtual_nodes = Config::VIRTUAL_NODES;

    uint32_t hash(const string& key) {
        // 使用标准库的哈希函数，分布更均匀
        return static_cast<uint32_t>(std::hash<string>{}(key));
    }

public:
    void addNode(const string& node) {
        nodes.push_back(node);
        for (int i = 0; i < virtual_nodes; i++) {
            string virtual_key = node + "#" + to_string(i);
            uint32_t hash_value = hash(virtual_key);
            ring[hash_value] = node;
        }
    }

    string getNode(const string& key) {
        if (ring.empty()) return "";
        
        uint32_t hash_value = hash(key);
        auto it = ring.lower_bound(hash_value);
        if (it == ring.end()) {
            it = ring.begin();
        }
        return it->second;
    }

    vector<string> getAllNodes() {
        return nodes;
    }
};

class CacheNode {
private:
    unordered_map<string, json> cache;
    shared_mutex cache_mutex;
    ConsistentHash consistent_hash;
    string node_id;
    int port;
    vector<string> all_nodes;
    string current_node_url;

public:
    CacheNode(const string& id, int p, const vector<string>& nodes) 
        : node_id(id), port(p), all_nodes(nodes) {
        // 初始化一致性哈希环
        for (const auto& node : nodes) {
            consistent_hash.addNode(node);
        }
        // 预计算当前节点URL
        current_node_url = "http://cache-server-" + to_string(port - Config::PORT_BASE) + ":" + to_string(port);
    }

    // 本地存储操作
    void setLocal(const string& key, const json& value) {
        unique_lock<shared_mutex> lock(cache_mutex);
        cache[key] = value;
    }

    json getLocal(const string& key) {
        shared_lock<shared_mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
        return json::value_t::null;
    }

    bool deleteLocal(const string& key) {
        unique_lock<shared_mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            cache.erase(it);
            return true;
        }
        return false;
    }

    // 获取目标节点
    string getTargetNode(const string& key) {
        return consistent_hash.getNode(key);
    }

    // 获取当前节点地址
    const string& getCurrentNode() {
        return current_node_url;
    }

    // HTTP响应辅助函数
    void setJsonResponse(httplib::Response& res, int status, const string& body) {
        res.status = status;
        res.set_header("Content-Type", "application/json; charset=utf-8");
        res.body = body;
    }

    void setErrorResponse(httplib::Response& res, int status, const string& error) {
        setJsonResponse(res, status, "{\"error\": \"" + error + "\"}");
    }

    void setSuccessResponse(httplib::Response& res, const string& body = "OK") {
        setJsonResponse(res, 200, body);
    }

    // RPC客户端工厂方法
    httplib::Client createRpcClient(const string& target_node) {
        httplib::Client client(target_node);
        client.set_connection_timeout(Config::RPC_TIMEOUT_SECONDS, 0);
        return client;
    }

    // 内部RPC调用
    json rpcGet(const string& target_node, const string& key) {
        auto client = createRpcClient(target_node);
        auto res = client.Get("/internal/get/" + key);
        if (res && res->status == 200) {
            try {
                return json::parse(res->body);
            } catch (const exception& e) {
                // JSON解析失败，返回null
            }
        }
        return json::value_t::null;
    }

    bool rpcSet(const string& target_node, const string& key, const json& value) {
        auto client = createRpcClient(target_node);
        json request;
        request[key] = value;
        
        auto res = client.Post("/internal/set", request.dump(), "application/json");
        return res && res->status == 200;
    }

    int rpcDelete(const string& target_node, const string& key) {
        auto client = createRpcClient(target_node);
        auto res = client.Delete("/internal/delete/" + key);
        if (res && res->status == 200) {
            try {
                return stoi(res->body);
            } catch (const exception& e) {
                // 响应解析失败，返回0
            }
        }
        return 0;
    }

    // 启动HTTP服务器
    void start() {
        httplib::Server server;

        // 设置CORS头
        server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // OPTIONS处理
        server.Options(".*", [](const httplib::Request&, httplib::Response&) {
            return;
        });

        // 健康检查接口 - 必须放在通用路由之前
        server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = "{\"status\":\"ok\",\"node\":\"" + node_id + "\"}";
        });

        // POST / - 写入/更新缓存
        server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (req.body.empty()) {
                    setErrorResponse(res, 400, "Empty request body");
                    return;
                }
                
                json body = json::parse(req.body);
                
                for (auto& item : body.items()) {
                    string key = item.key();
                    auto value = item.value();
                    string target_node = getTargetNode(key);
                    string current_node = getCurrentNode();
                    
                    if (target_node == current_node) {
                        // 数据应该存储在当前节点
                        setLocal(key, value);
                    } else {
                        // 数据应该存储在其他节点，通过RPC发送
                        if (!rpcSet(target_node, key, value)) {
                            setErrorResponse(res, 500, "Internal server error");
                            return;
                        }
                    }
                }
                
                setSuccessResponse(res);
            } catch (const exception& e) {
                setErrorResponse(res, 400, "Bad request: " + string(e.what()));
            }
        });

        // GET /{key} - 读取缓存
        server.Get(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                setErrorResponse(res, 400, "Invalid request");
                return;
            }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = getCurrentNode();
            
            json result;
            if (target_node == current_node) {
                // 数据在当前节点
                result = getLocal(key);
            } else {
                // 数据在其他节点，通过RPC获取
                result = rpcGet(target_node, key);
            }
            
            if (result.is_null()) {
                res.status = 404;
            } else {
                json response;
                response[key] = result;
                setJsonResponse(res, 200, response.dump());
            }
        });

        // DELETE /{key} - 删除缓存
        server.Delete(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                setErrorResponse(res, 400, "Invalid request");
                return;
            }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = getCurrentNode();
            
            int deleted = 0;
            if (target_node == current_node) {
                // 数据在当前节点
                deleted = deleteLocal(key) ? 1 : 0;
            } else {
                // 数据在其他节点，通过RPC删除
                deleted = rpcDelete(target_node, key);
            }
            
            setJsonResponse(res, 200, to_string(deleted));
        });

        // 内部RPC接口
        server.Get(R"(/internal/get/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                setErrorResponse(res, 400, "Invalid request");
                return;
            }
            string key = req.matches[0];
            json result = getLocal(key);
            
            if (result.is_null()) {
                res.status = 404;
            } else {
                setJsonResponse(res, 200, result.dump());
            }
        });

        server.Post("/internal/set", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                json body = json::parse(req.body);
                for (auto& item : body.items()) {
                    string key = item.key();
                    auto value = item.value();
                    setLocal(key, value);
                }
                setSuccessResponse(res);
            } catch (const exception& e) {
                setErrorResponse(res, 400, "Bad request: " + string(e.what()));
            }
        });

        server.Delete(R"(/internal/delete/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                setErrorResponse(res, 400, "Invalid request");
                return;
            }
            string key = req.matches[0];
            int deleted = deleteLocal(key) ? 1 : 0;
            setJsonResponse(res, 200, to_string(deleted));
        });


        cout << "缓存节点 " << node_id << " 启动在端口 " << port << endl;
        server.listen("0.0.0.0", port);
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "用法: " << argv[0] << " <端口号>" << endl;
        return 1;
    }

    int port = atoi(argv[1]);
    string node_id = "node" + to_string(port);
    
    // 配置所有节点 - 使用Docker服务名称进行容器间通信
    vector<string> all_nodes = {
        "http://cache-server-1:9527",
        "http://cache-server-2:9528", 
        "http://cache-server-3:9529"
    };

    CacheNode node(node_id, port, all_nodes);
    node.start();

    return 0;
}