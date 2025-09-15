#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <map>
#include <list>
#include <chrono>
#include <atomic>
#include "httplib.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

// ========================== NodeStats ==========================
struct NodeStats {
    atomic<int> request_count{0};
    atomic<int> success_count{0};
    atomic<int> error_count{0};
    double total_response_time{0.0};
    double avg_response_time{0.0};
    atomic<bool> is_healthy{true};
    time_point<steady_clock> last_check;
    time_point<steady_clock> last_request;
    mutable mutex time_mutex;  // 保护 double

    NodeStats() : last_check(steady_clock::now()), last_request(steady_clock::now()) {}

    void updateRequest(double response_time, bool success) {
        request_count++;
        if (success) success_count++;
        else error_count++;
        {
            lock_guard<mutex> lk(time_mutex);
            total_response_time += response_time;
            int total = request_count.load();
            if (total > 0) avg_response_time = total_response_time / total;
        }
        last_request = steady_clock::now();
    }

    double getErrorRate() const {
        int total = request_count.load();
        return total > 0 ? (double)error_count.load() / total : 0.0;
    }

    bool isHealthy() const {
        lock_guard<mutex> lk(time_mutex);
        return is_healthy.load() &&
               getErrorRate() < 0.3 &&
               avg_response_time < 1000.0 &&
               duration_cast<seconds>(steady_clock::now() - last_request).count() < 30;
    }
};


// ========================== ConsistentHash ==========================
class ConsistentHash {
private:
    map<uint32_t, string> ring;
    int virtual_nodes = 150;

    uint32_t hash(const string& key) {
        uint32_t h = 0;
        for (unsigned char c : key) h = h * 31 + c;
        return h;
    }

public:
    void addNode(const string& node) {
        for (int i = 0; i < virtual_nodes; ++i) {
            string vk = node + "#" + to_string(i);
            ring[hash(vk)] = node;
        }
    }

    string getNode(const string& key) {
        if (ring.empty()) return "";
        uint32_t hv = hash(key);
        auto it = ring.lower_bound(hv);
        if (it == ring.end()) it = ring.begin();
        return it->second;
    }
};

// ========================== CacheNode ==========================
class CacheNode {
private:
    unordered_map<string, json> cache;
    std::shared_mutex cache_mutex;
    ConsistentHash consistent_hash;
    string node_id;
    int port;
    vector<string> all_nodes;
    unordered_map<string, NodeStats> node_stats;
    mutex stats_mutex;

    static constexpr size_t MAX_CACHE_SIZE = 10000;
    list<string> lru_order;
    unordered_map<string, list<string>::iterator> lru_map;

public:
    CacheNode(const string& id, int p, const vector<string>& nodes)
        : node_id(id), port(p), all_nodes(nodes) {
        for (const auto& n : nodes) {
            consistent_hash.addNode(n);
            node_stats.try_emplace(n);
        }
    }

    string getCurrentNodeUrl() {
        return "http://127.0.0.1:" + to_string(port);
    }

    void setLocal(const string& key, const json& value) {
        if (cache.size() >= MAX_CACHE_SIZE && cache.find(key) == cache.end()) {
            string oldest = lru_order.back();
            lru_order.pop_back();
            lru_map.erase(oldest);
            cache.erase(oldest);
        }
        cache[key] = value;
        auto it = lru_map.find(key);
        if (it != lru_map.end()) {
            lru_order.splice(lru_order.begin(), lru_order, it->second);
            lru_map[key] = lru_order.begin();
        } else {
            lru_order.push_front(key);
            lru_map[key] = lru_order.begin();
        }
    }

    json getLocal(const string& key) {
        {
            std::shared_lock<shared_mutex> sl(cache_mutex);
            auto it = cache.find(key);
            if (it == cache.end()) return json::value_t::null;
            json val = it->second;
            sl.unlock();
            {
                std::unique_lock<shared_mutex> ul(cache_mutex);
                if (cache.find(key) != cache.end()) {
                    auto lru_it = lru_map.find(key);
                    if (lru_it != lru_map.end())
                        lru_order.splice(lru_order.begin(), lru_order, lru_it->second);
                }
            }
            return val;
        }
    }

    bool deleteLocal(const string& key) {
        unique_lock<shared_mutex> ul(cache_mutex);
        auto it = cache.find(key);
        if (it == cache.end()) return false;
        cache.erase(it);
        auto lru_it = lru_map.find(key);
        if (lru_it != lru_map.end()) {
            lru_order.erase(lru_it->second);
            lru_map.erase(lru_it);
        }
        return true;
    }

    // RPC wrappers
    unordered_map<string, unique_ptr<httplib::Client>> client_pool;
    mutex client_pool_mutex;

    httplib::Client* getClient(const string& target_node) {
        lock_guard<mutex> lk(client_pool_mutex);
        auto it = client_pool.find(target_node);
        if (it == client_pool.end()) {
            auto client = make_unique<httplib::Client>(target_node);
            client->set_connection_timeout(2, 0);
            client_pool[target_node] = move(client);
            return client_pool[target_node].get();
        }
        return it->second.get();
    }

    bool rpcSetBatch(const string& target_node, const json& kvs) {
        auto* client = getClient(target_node);
        auto res = client->Post("/internal/set", kvs.dump(), "application/json");
        return res && res->status == 200;
    }

    json rpcGet(const string& target_node, const string& key) {
        auto* client = getClient(target_node);
        auto res = client->Get("/internal/get/" + key);
        if (res && res->status == 200) {
            try { return json::parse(res->body); } catch (...) {}
        }
        return json::value_t::null;
    }

    int rpcDelete(const string& target_node, const string& key) {
        auto* client = getClient(target_node);
        auto res = client->Delete("/internal/delete/" + key);
        if (res && res->status == 200) {
            try { return stoi(res->body); } catch (...) {}
        }
        return 0;
    }

    string getTargetNode(const string& key) {
        string target = consistent_hash.getNode(key);
        return target;
    }

    // Start HTTP server
    void start() {
        httplib::Server server;

        server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.body.empty()) {
                res.status = 400;
                res.set_header("Content-Type", "application/json");
                res.body = "{\"error\":\"Empty body\"}";
                return;
            }
            json body = json::parse(req.body);
            unordered_map<string, json> local_requests;
            unordered_map<string, json> remote_requests;

            for (auto& item : body.items()) {
                string key = item.key();
                string target = getTargetNode(key);
                if (target == getCurrentNodeUrl()) {
                    local_requests[key] = item.value();
                } else {
                    remote_requests[target][key] = item.value();
                }
            }

            {
                unique_lock<shared_mutex> ul(cache_mutex);
                for (auto& kv : local_requests) setLocal(kv.first, kv.second);
            }

            for (auto& [target, kvs] : remote_requests) {
                rpcSetBatch(target, kvs);
            }

            res.status = 200;
            res.body = "OK";
        });

        server.Get(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            string key = req.matches[0];
            string target = getTargetNode(key);
            json val;
            if (target == getCurrentNodeUrl()) val = getLocal(key);
            else val = rpcGet(target, key);
            if (val.is_null()) { res.status = 404; return; }
            json out; out[key] = val;
            res.status = 200;
            res.set_header("Content-Type", "application/json");
            res.body = out.dump();
        });

        server.Delete(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            string key = req.matches[0];
            string target = getTargetNode(key);
            int deleted = 0;
            if (target == getCurrentNodeUrl()) deleted = deleteLocal(key) ? 1 : 0;
            else deleted = rpcDelete(target, key);
            res.status = 200;
            res.body = to_string(deleted);
        });

        server.Post("/internal/set", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body);
            unique_lock<shared_mutex> ul(cache_mutex);
            for (auto& item : body.items()) setLocal(item.key(), item.value());
            res.status = 200;
            res.body = "OK";
        });

        server.Get(R"(/internal/get/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            string key = req.matches[0];
            json val = getLocal(key);
            if (val.is_null()) { res.status = 404; return; }
            res.status = 200;
            res.body = val.dump();
        });

        server.Delete(R"(/internal/delete/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            string key = req.matches[0];
            int deleted = deleteLocal(key) ? 1 : 0;
            res.status = 200;
            res.body = to_string(deleted);
        });

        cout << "Cache node " << node_id << " listening on port " << port << endl;
        server.listen("0.0.0.0", port);
    }
};

// ========================== main ==========================
int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "用法: " << argv[0] << " <端口号>" << endl;
        return 1;
    }
    int port = atoi(argv[1]);
    string node_id = "node" + to_string(port);
    vector<string> all_nodes = {
        "http://127.0.0.1:9527",
        "http://127.0.0.1:9528",
        "http://127.0.0.1:9529"
    };

    CacheNode node(node_id, port, all_nodes);
    node.start();
    return 0;
}
