#include <iostream>
#include <fstream>
#include <memory>
#include <map>
#include <queue>

#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>

#include "proto/orderbook.pb.h"
#include "proto/orderbook.grpc.pb.h"
#include "proto/matcher.pb.h"
#include "proto/matcher.grpc.pb.h"

using namespace std;
using namespace proto;
using namespace grpc;

namespace OrderBook {
    typedef uint64_t Price;
    typedef uint64_t Size;
    typedef float_t Quote;
    typedef string_view UUID;

    enum Side { ASK, BID };

    class Order {
        UUID orderID;
        Side side;
        Price price;
        Size size;
        Price amount() { return price * size; }
    };

    class OrderQueue : public priority_queue<Order*> {
        Order* lookup(Order *m) const {
            auto result = find(c.begin(), c.end(), m);
            if (result != c.end()) { return *result; }
            return nullptr;
        }
        bool remove(Order *m) {
            if (auto it = lookup(m)) {
                c.erase(it);
                make_heap(c.begin(), c.end(), comp);
                return true;
            }
            return false;
        } 
        string_view string() const { string_view() };
    };

    class PriceNode {
        Side side;
        Depth depth;
        OrderQueue queue;
        bool insert(Order* m) {
            // Push to order queue
            this->depth += m->amount;
            queue.push(m);
            return true;
        }
        bool remove(Order* m) {
            // dequeue the order
            this->depth -= m->size;
            return queue.remove(m);
        }
    };

    class PriceTree : public map<Price, PriceNode> {
        bool insert(Order* m) {
            // Price level exists
            if (auto it = find(Price == m->Price)) {
                it->second.insert(m);
            } else { // Create new price point
                auto node = PriceNode();
                node
                insert({m->price, node)});
            }
        }
        string_view string() { string_view() };
    };

    class LimitOrderBook {
        PriceTree bidTree;
        PriceTree askTree;
        string_view market;
        void execute(Order* m) {
            // Handle order type and time in force constraint logic
            if (m->side == Side::BID) { // Process bid request
                if (askTree.find(m->price) != askTree.end()) {
                    // found => execute available amount or place leftover in the bid tree 
                } else {
                    // not found => attach new price node to bid tree
                }
            } else { // Process ask request
                if (bidTree.find(m->price) != bidTree.end()) {
                    // found
                } else {
                    // not found => attach new price node to ask tree
                }
            }
        }
        void cancel(Order* m) {
            if (m->side == Side::BID) {
                if (bidTree.find(m->price) == bidTree.end()) {
                    // not found
                } else {
                    // found
                }
            } else {

            }
        }
        vector<PriceNode> snapshot() const  {
            return vector<PriceNode>();            
        }
        PriceNode* lookup(const Price& p) const  {
            return find(begin(), end(), p);
        }
        Price bidPrice() const {
            return bidTree.begin()->first;
        }
        Price askPrice() const {
            return askTree.begin()->first;
        }
        float_t spread() const {
            Price bestAsk = askPrice();
            Price bestBid = bidPrice();
            return (bestAsk - bestBid) / bestAsk;
        }
        string_view string() const { return string_view(); }
    };
}

namespace MatchingEngine {
    class MatchingServer final : public MatcherProto::Service {
        public:
        Status GetOrderBook(ServerContext* ctx, const MarketRequestProto* request, OrderBookProto* reply) override {
            reply->set_market(request->market());
            cout << "GetOrderBook: " << request->market() << endl;
            auto point = reply->add_points();
            point->set_price(100);
            point->set_size(10);

            return Status::OK;
        }
    };
}

int main() {
    MatchingEngine::MatchingServer service;
    ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server(builder.BuildAndStart());
    cout << "Matching service is up and running!" << endl;
    server->Wait();

    return 0;
}
