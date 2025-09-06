#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <map>
#include "httplib.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;

class ConsistentHash {
private:
    map<uint32_t, string> ring;
    vector<string> nodes;
    int virtual_nodes = 150;

    uint32_t hash(const string& key) {
        // 简单的哈希函数
        uint32_t hash = 0;
        for (char c : key) {
            hash = hash * 31 + c;
        }
        return hash;
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
    mutex cache_mutex;
    ConsistentHash consistent_hash;
    string node_id;
    int port;
    vector<string> all_nodes;

public:
    CacheNode(const string& id, int p, const vector<string>& nodes) 
        : node_id(id), port(p), all_nodes(nodes) {
        // 初始化一致性哈希环
        for (const auto& node : nodes) {
            consistent_hash.addNode(node);
        }
    }

    // 本地存储操作
    void setLocal(const string& key, const json& value) {
        lock_guard<mutex> lock(cache_mutex);
        cache[key] = value;
    }

    json getLocal(const string& key) {
        lock_guard<mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
        return json::value_t::null;
    }

    bool deleteLocal(const string& key) {
        lock_guard<mutex> lock(cache_mutex);
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

    // 内部RPC调用
    json rpcGet(const string& target_node, const string& key) {
        httplib::Client client(target_node);
        client.set_connection_timeout(5, 0); // 5秒超时
        
        auto res = client.Get("/internal/get/" + key);
        if (res && res->status == 200) {
            try {
                return json::parse(res->body);
            } catch (const exception& e) {
                cout << "RPC GET 解析JSON失败: " << e.what() << endl;
            }
        }
        return json::value_t::null;
    }

    bool rpcSet(const string& target_node, const string& key, const json& value) {
        httplib::Client client(target_node);
        client.set_connection_timeout(5, 0);
        
        json request;
        request[key] = value;
        
        auto res = client.Post("/internal/set", request.dump(), "application/json");
        return res && res->status == 200;
    }

    int rpcDelete(const string& target_node, const string& key) {
        httplib::Client client(target_node);
        client.set_connection_timeout(5, 0);
        
        auto res = client.Delete("/internal/delete/" + key);
        if (res && res->status == 200) {
            try {
                return stoi(res->body);
            } catch (const exception& e) {
                cout << "RPC DELETE 解析响应失败: " << e.what() << endl;
            }
        }
        return 0;
    }

    // 启动HTTP服务器
    void start() {
        httplib::Server server;

        // 设置CORS头
        server.set_pre_routing_handler([](const httplib::Request& /* req */, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // OPTIONS处理
        server.Options(".*", [](const httplib::Request&, httplib::Response& /* res */) {
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
                // 调试信息
                cout << "收到POST请求，body长度: " << req.body.length() << endl;
                cout << "请求体内容: '" << req.body << "'" << endl;
                
                if (req.body.empty()) {
                    res.status = 400;
                    res.set_header("Content-Type", "application/json; charset=utf-8");
                    res.body = "{\"error\": \"Empty request body\"}";
                    return;
                }
                
                json body = json::parse(req.body);
                
                for (auto& item : body.items()) {
                    string key = item.key();
                    auto value = item.value();
                    string target_node = getTargetNode(key);
                    string current_node = "http://cache-server-" + to_string(port - 9526) + ":" + to_string(port);
                    
                    if (target_node == current_node) {
                        // 数据应该存储在当前节点
                        setLocal(key, value);
                    } else {
                        // 数据应该存储在其他节点，通过RPC发送
                        if (!rpcSet(target_node, key, value)) {
                            res.status = 500;
                            res.body = "Internal server error";
                            return;
                        }
                    }
                }
                
                res.status = 200;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = "OK";
            } catch (const exception& e) {
                res.status = 400;
                res.body = "Bad request: " + string(e.what());
            }
        });

        // GET /{key} - 读取缓存
        server.Get(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = "http://cache-server-" + to_string(port - 9526) + ":" + to_string(port);
            
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
                res.status = 200;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                json response;
                response[key] = result;
                res.body = response.dump();
            }
        });

        // DELETE /{key} - 删除缓存
        server.Delete(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = "http://cache-server-" + to_string(port - 9526) + ":" + to_string(port);
            
            int deleted = 0;
            if (target_node == current_node) {
                // 数据在当前节点
                deleted = deleteLocal(key) ? 1 : 0;
            } else {
                // 数据在其他节点，通过RPC删除
                deleted = rpcDelete(target_node, key);
            }
            
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = to_string(deleted);
        });

        // 内部RPC接口
        server.Get(R"(/internal/get/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            json result = getLocal(key);
            
            if (result.is_null()) {
                res.status = 404;
            } else {
                res.status = 200;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = result.dump();
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
                res.status = 200;
                res.body = "OK";
            } catch (const exception& e) {
                res.status = 400;
                res.body = "Bad request: " + string(e.what());
            }
        });

        server.Delete(R"(/internal/delete/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            int deleted = deleteLocal(key) ? 1 : 0;
            res.status = 200;
            res.body = to_string(deleted);
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