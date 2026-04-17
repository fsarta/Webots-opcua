#include <webots/Robot.hpp>
#include <webots/Supervisor.hpp>
#include <webots/Node.hpp>
#include <webots/Field.hpp>
#include <webots/Motor.hpp>
#include <webots/PositionSensor.hpp>
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <map>

extern "C" {
    #include "open62541.h"
}

using namespace webots;

struct DiscoveredTag {
    std::string name;
    std::string nodeId;
    std::string path;
};

struct MappingConfig {
    std::string dir;
    std::string target;
    std::string param;
    int sampling = 100;
};

struct OpcUaContext {
    std::atomic<bool> running{true};
    std::atomic<bool> connect_requested{false};
    std::atomic<bool> disconnect_requested{false};
    std::string endpoint_url;
    double publishing_interval = 10.0;
    std::atomic<bool> connected{false};
    std::atomic<bool> connection_error{false};
    
    std::vector<DiscoveredTag> discovered_tags;
    std::atomic<bool> tags_ready{false};

    std::vector<std::pair<std::string, int>> pending_subscribes; 
    std::vector<std::string> pending_unsubscribes;
    
    std::unordered_map<std::string, double> opc_to_webots_values; 
    std::unordered_map<std::string, double> webots_to_opc_values; 
    
    std::mutex mutex;
    UA_Client *client = nullptr;
    UA_UInt32 subscriptionId = 0;
    
    std::map<std::string, UA_UInt32> monitored_items;
    std::unordered_map<std::string, std::string> nodeId_to_name;
};

OpcUaContext opcua_ctx;

std::vector<std::string> splitString(const std::string &s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (getline(ss, item, delim)) result.push_back(item);
    return result;
}

void saveMappingToFile(const std::string& endpoint, double pubInt, const std::unordered_map<std::string, MappingConfig>& mappings) {
    std::ofstream outFile("opcua_mapping.config");
    if (outFile.is_open()) {
        outFile << "ENDPOINT|" << endpoint << "|" << pubInt << "\n";
        for (const auto& pair : mappings) {
            outFile << "MAP|" << pair.first << "|" << pair.second.dir << "|" << pair.second.target << "|" << pair.second.param << "|" << pair.second.sampling << "\n";
        }
        outFile.close();
    }
}

void loadMappingFromFile(std::string& endpoint, double& pubInt, std::unordered_map<std::string, MappingConfig>& mappings) {
    std::ifstream inFile("opcua_mapping.config");
    if (inFile.is_open()) {
        std::string line;
        mappings.clear();
        while (std::getline(inFile, line)) {
            auto parts = splitString(line, '|');
            if (parts.size() >= 2 && parts[0] == "ENDPOINT") {
                endpoint = parts[1];
                if (parts.size() > 2) pubInt = std::stod(parts[2]);
            } else if (parts.size() >= 5 && parts[0] == "MAP") {
                MappingConfig cfg = { parts[2], parts[3], parts[4], (parts.size() > 5 ? std::stoi(parts[5]) : 100) };
                mappings[parts[1]] = cfg;
            }
        }
        inFile.close();
    }
}

std::string nodeIdToString(const UA_NodeId& nodeId) {
    if (nodeId.identifierType == UA_NODEIDTYPE_NUMERIC) return "ns=" + std::to_string(nodeId.namespaceIndex) + ";i=" + std::to_string(nodeId.identifier.numeric);
    else if (nodeId.identifierType == UA_NODEIDTYPE_STRING) return "ns=" + std::to_string(nodeId.namespaceIndex) + ";s=" + std::string((char*)nodeId.identifier.string.data, nodeId.identifier.string.length);
    return "Unsupported_NodeId";
}

UA_NodeId parseNodeIdString(const std::string& str) {
    UA_NodeId id = UA_NODEID_NULL;
    if (str.find("ns=") == 0) {
        size_t semi = str.find(';');
        if (semi != std::string::npos) {
            int ns = std::stoi(str.substr(3, semi - 3));
            char type = str[semi + 1];
            std::string valStr = str.substr(semi + 3);
            if (type == 'i') id = UA_NODEID_NUMERIC(ns, std::stoi(valStr));
            else if (type == 's') id = UA_NODEID_STRING_ALLOC(ns, valStr.c_str());
        }
    }
    return id;
}

void dataChangeNotificationCallback(UA_Client *client, UA_UInt32 subId, void *subContext, UA_UInt32 monId, void *monContext, UA_DataValue *value) {
    if (value->hasValue && monContext != nullptr) {
        std::string nodeIdStr = static_cast<const char*>(monContext);
        double numValue = 0.0; bool valid = false;
        if (UA_Variant_isScalar(&value->value)) {
            if (value->value.type == &UA_TYPES[UA_TYPES_DOUBLE]) { numValue = *(UA_Double*)value->value.data; valid = true; }
            else if (value->value.type == &UA_TYPES[UA_TYPES_FLOAT]) { numValue = (double)(*(UA_Float*)value->value.data); valid = true; }
            else if (value->value.type == &UA_TYPES[UA_TYPES_INT32]) { numValue = (double)(*(UA_Int32*)value->value.data); valid = true; }
            else if (value->value.type == &UA_TYPES[UA_TYPES_BOOLEAN]) { numValue = *(UA_Boolean*)value->value.data ? 1.0 : 0.0; valid = true; }
        }
        if (valid) { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); opcua_ctx.opc_to_webots_values[nodeIdStr] = numValue; }
    }
}

void browseRecursive(UA_Client *client, UA_NodeId startNodeId, std::string currentPath, std::vector<DiscoveredTag>& tags, int depth = 0) {
    if (depth > 5) return;
    UA_BrowseRequest bReq; UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    UA_NodeId_copy(&startNodeId, &bReq.nodesToBrowse[0].nodeId);
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);
    for (size_t i = 0; i < bResp.resultsSize; ++i) {
        for (size_t j = 0; j < bResp.results[i].referencesSize; ++j) {
            UA_ReferenceDescription *ref = &(bResp.results[i].references[j]);
            if (ref->nodeClass == UA_NODECLASS_VARIABLE) {
                DiscoveredTag tag;
                tag.name = std::string((char*)ref->browseName.name.data, ref->browseName.name.length);
                tag.nodeId = nodeIdToString(ref->nodeId.nodeId);
                tag.path = currentPath + "/" + tag.name;
                tags.push_back(tag);
            } else if (ref->nodeClass == UA_NODECLASS_OBJECT || ref->nodeClass == UA_NODECLASS_VARIABLETYPE) {
                std::string childName = std::string((char*)ref->browseName.name.data, ref->browseName.name.length);
                browseRecursive(client, ref->nodeId.nodeId, currentPath + "/" + childName, tags, depth + 1);
            }
        }
    }
    UA_BrowseRequest_clear(&bReq); UA_BrowseResponse_clear(&bResp);
}

void opcuaWorkerThread() {
    opcua_ctx.client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(opcua_ctx.client));
    auto last_reconnect_attempt = std::chrono::steady_clock::now();

    while (opcua_ctx.running) {
        std::string current_url; double current_pub = 10.0; bool do_connect = false;
        {
            std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
<<<<<<< HEAD
            if (opcua_ctx.connect_requested) { current_url = opcua_ctx.endpoint_url; current_pub = opcua_ctx.publishing_interval; opcua_ctx.connect_requested = false; opcua_ctx.disconnect_requested = false; do_connect = true; }
            else if (opcua_ctx.disconnect_requested) { if (opcua_ctx.connected) { UA_Client_disconnect(opcua_ctx.client); opcua_ctx.connected = false; opcua_ctx.connection_error = false; } opcua_ctx.disconnect_requested = false; }
            else if (!opcua_ctx.connected && !opcua_ctx.endpoint_url.empty() && opcua_ctx.connection_error) {
=======
            if (opcua_ctx.disconnect_requested) {
                if (opcua_ctx.connected) {
                    UA_Client_disconnect(opcua_ctx.client);
                    opcua_ctx.connected = false;
                    opcua_ctx.connection_error = false;
                    std::cout << "[OPC-UA Thread] Disconnessione manuale." << std::endl;
                }
                opcua_ctx.disconnect_requested = false;
            } else if (opcua_ctx.connect_requested) {
                current_url = opcua_ctx.endpoint_url;
                opcua_ctx.connect_requested = false;
                do_connect = true;
            } else if (!opcua_ctx.connected && !opcua_ctx.endpoint_url.empty() && opcua_ctx.connection_error) {
                // Logica di Auto-Riconnessione (Watchdog: prova ogni 2 secondi)
>>>>>>> 8d7cd4ef42b30c6de0e8ef212ae1e9bb3e6beb41
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_reconnect_attempt).count() > 2) { current_url = opcua_ctx.endpoint_url; current_pub = opcua_ctx.publishing_interval; do_connect = true; last_reconnect_attempt = now; }
            }
        }

        if (do_connect) {
            if (UA_Client_connect(opcua_ctx.client, current_url.c_str()) == UA_STATUSCODE_GOOD) {
                opcua_ctx.connected = true; opcua_ctx.connection_error = false;
                UA_CreateSubscriptionRequest request = UA_CreateSubscriptionRequest_default();
                request.requestedPublishingInterval = current_pub; 
                UA_CreateSubscriptionResponse response = UA_Client_Subscriptions_create(opcua_ctx.client, request, NULL, nullptr, nullptr);
                opcua_ctx.subscriptionId = response.subscriptionId;
                std::vector<DiscoveredTag> tags;
                browseRecursive(opcua_ctx.client, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), "Root", tags);
                {
                    std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                    opcua_ctx.discovered_tags = tags; opcua_ctx.tags_ready = true;
                    opcua_ctx.nodeId_to_name.clear();
                    for (const auto& t : tags) { opcua_ctx.nodeId_to_name[t.nodeId] = t.name; opcua_ctx.pending_subscribes.push_back({t.nodeId, 500}); }
                }
            } else { opcua_ctx.connected = false; opcua_ctx.connection_error = true; }
        }
        
        if (opcua_ctx.connected) {
            std::vector<std::pair<std::string, int>> subs; std::vector<std::string> unsubs; std::unordered_map<std::string, double> to_write;
            { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); subs = opcua_ctx.pending_subscribes; unsubs = opcua_ctx.pending_unsubscribes; opcua_ctx.pending_subscribes.clear(); opcua_ctx.pending_unsubscribes.clear(); to_write = opcua_ctx.webots_to_opc_values; opcua_ctx.webots_to_opc_values.clear(); }
            for (const auto& sub : subs) {
                auto it = opcua_ctx.monitored_items.find(sub.first);
                if (it != opcua_ctx.monitored_items.end()) { UA_Client_MonitoredItems_deleteSingle(opcua_ctx.client, opcua_ctx.subscriptionId, it->second); opcua_ctx.monitored_items.erase(it); }
                UA_NodeId id = parseNodeIdString(sub.first);
                UA_MonitoredItemCreateRequest monRequest = UA_MonitoredItemCreateRequest_default(id);
                monRequest.requestedParameters.samplingInterval = (double)sub.second;
                void* contextStr = nullptr;
                { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); auto itN = opcua_ctx.nodeId_to_name.find(sub.first); if (itN != opcua_ctx.nodeId_to_name.end()) contextStr = (void*)itN->first.c_str(); }
                UA_MonitoredItemCreateResult monResponse = UA_Client_MonitoredItems_createDataChange(opcua_ctx.client, opcua_ctx.subscriptionId, UA_TIMESTAMPSTORETURN_BOTH, monRequest, contextStr, dataChangeNotificationCallback, nullptr);
                if (monResponse.statusCode == UA_STATUSCODE_GOOD) opcua_ctx.monitored_items[sub.first] = monResponse.monitoredItemId;
                UA_NodeId_clear(&id);
            }
            for (const auto& nodeIdStr : unsubs) {
                auto it = opcua_ctx.monitored_items.find(nodeIdStr);
                if (it != opcua_ctx.monitored_items.end()) { UA_Client_MonitoredItems_deleteSingle(opcua_ctx.client, opcua_ctx.subscriptionId, it->second); opcua_ctx.monitored_items.erase(it); }
            }
            for (const auto& pair : to_write) {
                UA_NodeId id = parseNodeIdString(pair.first);
                UA_Variant value; UA_Variant_init(&value);
                UA_Double valDouble = pair.second; UA_Variant_setScalar(&value, &valDouble, &UA_TYPES[UA_TYPES_DOUBLE]);
                UA_Client_writeValueAttribute(opcua_ctx.client, id, &value);
                UA_NodeId_clear(&id);
            }
            if (UA_Client_run_iterate(opcua_ctx.client, 10) != UA_STATUSCODE_GOOD) { opcua_ctx.connected = false; opcua_ctx.connection_error = true; }
        } else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (opcua_ctx.connected) UA_Client_disconnect(opcua_ctx.client);
    UA_Client_delete(opcua_ctx.client);
}

int main(int argc, char **argv) {
    Supervisor *supervisor = new Supervisor();
    int timeStep = (int)supervisor->getBasicTimeStep();
    std::thread opcua_thread(opcuaWorkerThread);
    bool last_connected = false; bool last_error = false;
    std::unordered_map<std::string, MappingConfig> active_mappings;
    loadMappingFromFile(opcua_ctx.endpoint_url, opcua_ctx.publishing_interval, active_mappings);

    while (supervisor->step(timeStep) != -1) {
        std::string message = supervisor->wwiReceiveText();
        while (!message.empty()) {
            if (message.rfind("CONNECT:", 0) == 0) {
                auto parts = splitString(message.substr(8), '|');
                std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                opcua_ctx.endpoint_url = parts[0];
                if (parts.size() > 1) opcua_ctx.publishing_interval = std::stod(parts[1]);
                opcua_ctx.connect_requested = true;
            } 
            else if (message == "DISCONNECT") { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); opcua_ctx.disconnect_requested = true; }
            else if (message == "SAVE_CONFIG") saveMappingToFile(opcua_ctx.endpoint_url, opcua_ctx.publishing_interval, active_mappings);
            else if (message == "LOAD_CONFIG") {
                loadMappingFromFile(opcua_ctx.endpoint_url, opcua_ctx.publishing_interval, active_mappings);
                for (const auto& pair : active_mappings) {
                    if (pair.second.dir == "OPC_TO_WEBOTS") { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); opcua_ctx.pending_subscribes.push_back({pair.first, pair.second.sampling}); }
                    else if (pair.second.dir == "WEBOTS_TO_OPC" && pair.second.param == "SENSOR_VAL") { PositionSensor *s = supervisor->getPositionSensor(pair.second.target); if (s) s->enable(timeStep); }
                }
                std::stringstream ss; ss << "CONFIG_URL:" << opcua_ctx.endpoint_url << "|" << opcua_ctx.publishing_interval;
                supervisor->wwiSendText(ss.str());
            }
            else if (message.rfind("MAP:", 0) == 0) {
                auto parts = splitString(message.substr(4), '|');
                if (parts.size() >= 4) {
                    int sampling = (parts.size() > 4 ? std::stoi(parts[4]) : 100);
                    MappingConfig cfg = { parts[1], parts[2], parts[3], sampling };
                    active_mappings[parts[0]] = cfg;
                    if (cfg.dir == "OPC_TO_WEBOTS") { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); opcua_ctx.pending_subscribes.push_back({parts[0], sampling}); }
                    if (cfg.dir == "WEBOTS_TO_OPC" && cfg.param == "SENSOR_VAL") { PositionSensor *s = supervisor->getPositionSensor(cfg.target); if (s) s->enable(timeStep); }
                }
            } 
            else if (message.rfind("UNMAP:", 0) == 0) {
                std::string nodeId = message.substr(6);
                if (active_mappings.count(nodeId)) {
                    if (active_mappings[nodeId].dir == "WEBOTS_TO_OPC" && active_mappings[nodeId].param == "SENSOR_VAL") { PositionSensor *s = supervisor->getPositionSensor(active_mappings[nodeId].target); if (s) s->disable(); }
                    active_mappings.erase(nodeId);
                    std::lock_guard<std::mutex> lock(opcua_ctx.mutex); opcua_ctx.pending_subscribes.push_back({nodeId, 500});
                }
            }
            message = supervisor->wwiReceiveText();
        }

        if (opcua_ctx.connected != last_connected) { supervisor->wwiSendText(opcua_ctx.connected ? "STATUS:CONNECTED" : "STATUS:DISCONNECTED"); last_connected = opcua_ctx.connected; }
        if (opcua_ctx.connection_error && !last_error) { supervisor->wwiSendText("STATUS:ERROR"); last_error = true; } else if (!opcua_ctx.connection_error) last_error = false;

        if (opcua_ctx.tags_ready) {
            std::vector<DiscoveredTag> tags_copy; { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); tags_copy = opcua_ctx.discovered_tags; opcua_ctx.tags_ready = false; }
            std::stringstream json; json << "TAGS:[";
            for (size_t i = 0; i < tags_copy.size(); ++i) { json << "{\"name\":\"" << tags_copy[i].name << "\", \"nodeId\":\"" << tags_copy[i].nodeId << "\", \"path\":\"" << tags_copy[i].path << "\"}"; if (i < tags_copy.size() - 1) json << ","; }
            json << "]"; supervisor->wwiSendText(json.str());
        }

        static double last_val_send = 0;
        if (supervisor->getTime() - last_val_send > 0.2) {
            std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
            if (!opcua_ctx.opc_to_webots_values.empty()) {
                std::stringstream json; json << "VALUES:{";
                for (auto it = opcua_ctx.opc_to_webots_values.begin(); it != opcua_ctx.opc_to_webots_values.end(); ++it) { json << "\"" << it->first << "\":" << it->second; if (std::next(it) != opcua_ctx.opc_to_webots_values.end()) json << ","; }
                json << "}"; supervisor->wwiSendText(json.str());
            }
            last_val_send = supervisor->getTime();
        }

        static double last_map_send = 0;
        if (supervisor->getTime() - last_map_send > 1.0) {
            std::stringstream json; json << "MAPPINGS:{";
            for (auto it = active_mappings.begin(); it != active_mappings.end(); ++it) { json << "\"" << it->first << "\":{\"dir\":\"" << it->second.dir << "\", \"target\":\"" << it->second.target << "\", \"param\":\"" << it->second.param << "\", \"sampling\":" << it->second.sampling << "}"; if (std::next(it) != active_mappings.end()) json << ","; }
            json << "}"; supervisor->wwiSendText(json.str());
            last_map_send = supervisor->getTime();
        }

        std::unordered_map<std::string, double> in_vals; { std::lock_guard<std::mutex> lock(opcua_ctx.mutex); in_vals = opcua_ctx.opc_to_webots_values; }
        for (const auto& pair : active_mappings) {
            if (pair.second.dir == "OPC_TO_WEBOTS" && in_vals.count(pair.first)) {
                double val = in_vals[pair.first];
                if (pair.second.param == "MOTOR_POS") { Motor *m = supervisor->getMotor(pair.second.target); if(m) m->setPosition(val); }
                else if (pair.second.param == "MOTOR_VEL") { Motor *m = supervisor->getMotor(pair.second.target); if(m) { m->setPosition(INFINITY); m->setVelocity(val); } }
                else if (pair.second.param.find("TRANS_") == 0) {
                    Node *n = supervisor->getFromDef(pair.second.target);
                    if (n) { Field *f = n->getField("translation"); if (f) { const double *c = f->getSFVec3f(); double next[3] = {c[0],c[1],c[2]}; if (pair.second.param == "TRANS_X") next[0] = val; else if (pair.second.param == "TRANS_Y") next[1] = val; else if (pair.second.param == "TRANS_Z") next[2] = val; f->setSFVec3f(next); } }
                }
            } else if (pair.second.dir == "WEBOTS_TO_OPC") {
                double out_val = 0.0;
                if (pair.second.param == "SENSOR_VAL") { PositionSensor *ps = supervisor->getPositionSensor(pair.second.target); if (ps) out_val = ps->getValue(); }
                else if (pair.second.param.find("TRANS_") == 0) { Node *n = supervisor->getFromDef(pair.second.target); if (n) { Field *f = n->getField("translation"); if (f) { const double *c = f->getSFVec3f(); if (pair.second.param == "TRANS_X") out_val = c[0]; else if (pair.second.param == "TRANS_Y") out_val = c[1]; else if (pair.second.param == "TRANS_Z") out_val = c[2]; } } }
                std::lock_guard<std::mutex> lock(opcua_ctx.mutex); opcua_ctx.webots_to_opc_values[pair.first] = out_val;
            }
        }
    }
<<<<<<< HEAD
    opcua_ctx.running = false; if (opcua_thread.joinable()) opcua_thread.join();
    delete supervisor; return 0;
=======

    opcua_ctx.running = false;
    if (opcua_thread.joinable()) {
        opcua_thread.join();
    }

    delete supervisor;
    return 0;
>>>>>>> 8d7cd4ef42b30c6de0e8ef212ae1e9bb3e6beb41
}