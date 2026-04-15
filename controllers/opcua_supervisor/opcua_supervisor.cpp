#include <webots/Robot.hpp>
#include <webots/Supervisor.hpp>
#include <webots/Node.hpp>
#include <webots/Field.hpp>
#include <webots/Motor.hpp>
#include <webots/PositionSensor.hpp>
#include <iostream>
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

// Strutture dati
struct DiscoveredTag {
    std::string name;
    std::string nodeId;
};

struct MappingConfig {
    std::string dir;    // "OPC_TO_WEBOTS" o "WEBOTS_TO_OPC"
    std::string target; // Nome del device (es. "motor_1") o DEF del nodo
    std::string param;  // "MOTOR_POS", "TRANS_X", "SENSOR_VAL", ecc.
};

struct OpcUaContext {
    std::atomic<bool> running{true};
    
    // Connessione e Watchdog
    std::atomic<bool> connect_requested{false};
    std::string endpoint_url;
    std::atomic<bool> connected{false};
    std::atomic<bool> connection_error{false};
    
    // Discovery
    std::vector<DiscoveredTag> discovered_tags;
    std::atomic<bool> tags_ready{false};

    // Code di mappatura
    std::vector<std::string> pending_subscribes;
    std::vector<std::string> pending_unsubscribes;
    
    // Mappe dati in tempo reale (protette da mutex)
    std::unordered_map<std::string, double> opc_to_webots_values; // Dati scritti dal server OPC, letti da Webots
    std::unordered_map<std::string, double> webots_to_opc_values; // Dati letti dai sensori Webots, da inviare al server OPC
    
    std::mutex mutex;
    UA_Client *client = nullptr;
    UA_UInt32 subscriptionId = 0;
    
    // Stato interno thread OPC
    std::map<std::string, UA_UInt32> monitored_items;
    std::unordered_map<std::string, std::string> nodeId_to_name;
};

OpcUaContext opcua_ctx;

// --- FUNZIONI DI SUPPORTO ---

std::vector<std::string> splitString(const std::string &s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (getline(ss, item, delim)) {
        result.push_back(item);
    }
    return result;
}

std::string nodeIdToString(const UA_NodeId& nodeId) {
    if (nodeId.identifierType == UA_NODEIDTYPE_NUMERIC) {
        return "ns=" + std::to_string(nodeId.namespaceIndex) + ";i=" + std::to_string(nodeId.identifier.numeric);
    } else if (nodeId.identifierType == UA_NODEIDTYPE_STRING) {
        return "ns=" + std::to_string(nodeId.namespaceIndex) + ";s=" + 
               std::string((char*)nodeId.identifier.string.data, nodeId.identifier.string.length);
    }
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
            if (type == 'i') {
                id = UA_NODEID_NUMERIC(ns, std::stoi(valStr));
            } else if (type == 's') {
                id = UA_NODEID_STRING_ALLOC(ns, valStr.c_str());
            }
        }
    }
    return id;
}

// Callback invocata quando OPC-UA notifica un nuovo valore
void dataChangeNotificationCallback(UA_Client *client, UA_UInt32 subId, void *subContext,
                                    UA_UInt32 monId, void *monContext, UA_DataValue *value) {
    if (value->hasValue && monContext != nullptr) {
        double numValue = 0.0;
        bool valid = false;
        
        if (UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_DOUBLE])) {
            numValue = *(UA_Double*)value->value.data; valid = true;
        } else if (UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT])) {
            numValue = *(UA_Float*)value->value.data; valid = true;
        } else if (UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_INT32])) {
            numValue = *(UA_Int32*)value->value.data; valid = true;
        } else if (UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_INT16])) {
            numValue = *(UA_Int16*)value->value.data; valid = true;
        } else if (UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_BOOLEAN])) {
            numValue = *(UA_Boolean*)value->value.data ? 1.0 : 0.0; valid = true;
        }

        if (valid) {
            std::string nodeIdStr = static_cast<const char*>(monContext);
            std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
            opcua_ctx.opc_to_webots_values[nodeIdStr] = numValue;
        }
    }
}

void browseRecursive(UA_Client *client, UA_NodeId startNodeId, std::vector<DiscoveredTag>& tags, int depth = 0) {
    if (depth > 5) return;
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = startNodeId;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);
    for (size_t i = 0; i < bResp.resultsSize; ++i) {
        for (size_t j = 0; j < bResp.results[i].referencesSize; ++j) {
            UA_ReferenceDescription *ref = &(bResp.results[i].references[j]);
            if (ref->nodeClass == UA_NODECLASS_VARIABLE) {
                DiscoveredTag tag;
                tag.name = std::string((char*)ref->browseName.name.data, ref->browseName.name.length);
                tag.nodeId = nodeIdToString(ref->nodeId.nodeId);
                tags.push_back(tag);
            } else if (ref->nodeClass == UA_NODECLASS_OBJECT || ref->nodeClass == UA_NODECLASS_VARIABLETYPE) {
                browseRecursive(client, ref->nodeId.nodeId, tags, depth + 1);
            }
        }
    }
    UA_BrowseRequest_clear(&bReq);
    UA_BrowseResponse_clear(&bResp);
}


// --- THREAD OPC-UA IN BACKGROUND ---

void opcuaWorkerThread() {
    std::cout << "[OPC-UA Thread] Avviato in background." << std::endl;
    opcua_ctx.client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(opcua_ctx.client));
    
    auto last_reconnect_attempt = std::chrono::steady_clock::now();

    while (opcua_ctx.running) {
        std::string current_url;
        bool do_connect = false;

        {
            std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
            if (opcua_ctx.connect_requested) {
                current_url = opcua_ctx.endpoint_url;
                opcua_ctx.connect_requested = false;
                do_connect = true;
            } else if (!opcua_ctx.connected && !opcua_ctx.endpoint_url.empty() && opcua_ctx.connection_error) {
                // Logica di Auto-Riconnessione (Watchdog: prova ogni 2 secondi)
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_reconnect_attempt).count() > 2) {
                    current_url = opcua_ctx.endpoint_url;
                    do_connect = true;
                    last_reconnect_attempt = now;
                }
            }
        }

        if (do_connect) {
            std::cout << "[OPC-UA Thread] Tentativo di connessione a: " << current_url << std::endl;
            UA_StatusCode retval = UA_Client_connect(opcua_ctx.client, current_url.c_str());
            
            if (retval == UA_STATUSCODE_GOOD) {
                opcua_ctx.connected = true;
                opcua_ctx.connection_error = false;
                std::cout << "[OPC-UA Thread] Connessione Riuscita!" << std::endl;
                
                // Crea la Subscription
                UA_CreateSubscriptionRequest request = UA_CreateSubscriptionRequest_default();
                request.requestedPublishingInterval = 50.0;
                UA_CreateSubscriptionResponse response = UA_Client_Subscriptions_create(opcua_ctx.client, request, NULL, nullptr, nullptr);
                opcua_ctx.subscriptionId = response.subscriptionId;

                // Discovery
                std::vector<DiscoveredTag> tags;
                browseRecursive(opcua_ctx.client, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), tags);
                {
                    std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                    opcua_ctx.discovered_tags = tags;
                    opcua_ctx.tags_ready = true;
                    opcua_ctx.nodeId_to_name.clear();
                    for (const auto& t : tags) opcua_ctx.nodeId_to_name[t.nodeId] = t.name;
                    
                    // Ri-sottoscrivi ai nodi precedenti in caso di riconnessione
                    for (const auto& pair : opcua_ctx.monitored_items) {
                        opcua_ctx.pending_subscribes.push_back(pair.first);
                    }
                    opcua_ctx.monitored_items.clear();
                }
            } else {
                opcua_ctx.connected = false;
                opcua_ctx.connection_error = true;
            }
        }
        
        if (opcua_ctx.connected) {
            // 1. Gestione Richieste di Sottoscrizione e Annullamento (OPC_TO_WEBOTS)
            std::vector<std::string> subs, unsubs;
            std::unordered_map<std::string, double> to_write;
            {
                std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                subs = opcua_ctx.pending_subscribes;
                unsubs = opcua_ctx.pending_unsubscribes;
                opcua_ctx.pending_subscribes.clear();
                opcua_ctx.pending_unsubscribes.clear();
                
                // Preleva i valori da scrivere in OPC-UA dal thread Webots
                to_write = opcua_ctx.webots_to_opc_values;
                opcua_ctx.webots_to_opc_values.clear(); // Svuota la coda di scrittura
            }

            for (const auto& nodeIdStr : subs) {
                if (opcua_ctx.monitored_items.find(nodeIdStr) == opcua_ctx.monitored_items.end()) {
                    UA_NodeId id = parseNodeIdString(nodeIdStr);
                    UA_MonitoredItemCreateRequest monRequest = UA_MonitoredItemCreateRequest_default(id);
                    
                    void* contextStr = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                        auto it = opcua_ctx.nodeId_to_name.find(nodeIdStr);
                        if (it != opcua_ctx.nodeId_to_name.end()) contextStr = (void*)it->first.c_str();
                    }

                    UA_MonitoredItemCreateResult monResponse = 
                        UA_Client_MonitoredItems_createDataChange(opcua_ctx.client, opcua_ctx.subscriptionId, 
                                                                UA_TIMESTAMPSTORETURN_BOTH, monRequest, 
                                                                contextStr, dataChangeNotificationCallback, nullptr);
                    
                    if (monResponse.statusCode == UA_STATUSCODE_GOOD) {
                        opcua_ctx.monitored_items[nodeIdStr] = monResponse.monitoredItemId;
                    }
                    UA_NodeId_clear(&id);
                }
            }

            for (const auto& nodeIdStr : unsubs) {
                auto it = opcua_ctx.monitored_items.find(nodeIdStr);
                if (it != opcua_ctx.monitored_items.end()) {
                    UA_Client_MonitoredItems_deleteSingle(opcua_ctx.client, opcua_ctx.subscriptionId, it->second);
                    opcua_ctx.monitored_items.erase(it);
                    std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                    opcua_ctx.opc_to_webots_values.erase(nodeIdStr);
                }
            }

            // 2. Scrittura Valori (WEBOTS_TO_OPC)
            for (const auto& pair : to_write) {
                UA_NodeId id = parseNodeIdString(pair.first);
                UA_Variant value;
                UA_Variant_init(&value);
                UA_Double valDouble = pair.second;
                // Scriviamo forzando come Double. Per Server molto restrittivi potrebbe servire leggere prima il DataType.
                UA_Variant_setScalar(&value, &valDouble, &UA_TYPES[UA_TYPES_DOUBLE]);
                
                UA_StatusCode retval = UA_Client_writeValueAttribute(opcua_ctx.client, id, &value);
                if (retval == UA_STATUSCODE_BADCONNECTIONCLOSED) {
                    opcua_ctx.connected = false;
                    opcua_ctx.connection_error = true;
                    break; // Esci dal ciclo, riconnetterà
                }
                UA_NodeId_clear(&id);
            }

            // 3. Elabora callback di rete
            UA_StatusCode status = UA_Client_run_iterate(opcua_ctx.client, 10);
            if (status == UA_STATUSCODE_BADCONNECTIONCLOSED || status == UA_STATUSCODE_BADSERVERNOTCONNECTED) {
                std::cout << "[OPC-UA Thread] Connessione persa! Avvio Watchdog..." << std::endl;
                opcua_ctx.connected = false;
                opcua_ctx.connection_error = true;
            }

        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    if (opcua_ctx.connected) {
        UA_Client_disconnect(opcua_ctx.client);
    }
    UA_Client_delete(opcua_ctx.client);
    std::cout << "[OPC-UA Thread] Terminato." << std::endl;
}


// --- MAIN LOOP DI WEBOTS ---

int main(int argc, char **argv) {
    Supervisor *supervisor = new Supervisor();
    int timeStep = (int)supervisor->getBasicTimeStep();
    std::cout << "[Main Thread] Supervisor OPC-UA Inizializzato. TimeStep: " << timeStep << "ms" << std::endl;

    std::thread opcua_thread(opcuaWorkerThread);

    bool last_connected = false;
    bool last_error = false;
    
    std::unordered_map<std::string, MappingConfig> active_mappings;

    while (supervisor->step(timeStep) != -1) {
        
        // 1. GESTIONE MESSAGGI UI
        std::string message = supervisor->wwiReceiveText();
        while (!message.empty()) {
            if (message.rfind("CONNECT:", 0) == 0) {
                std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                opcua_ctx.endpoint_url = message.substr(8);
                opcua_ctx.connect_requested = true;
                opcua_ctx.connected = false;
                opcua_ctx.connection_error = false;
            } 
            else if (message.rfind("MAP:", 0) == 0) {
                // Formato: MAP:nodeId|DIR|TARGET|PARAM
                std::string data = message.substr(4);
                auto parts = splitString(data, '|');
                if (parts.size() == 4) {
                    MappingConfig cfg = { parts[1], parts[2], parts[3] };
                    active_mappings[parts[0]] = cfg;
                    
                    // Se è da OPC verso Webots, mi devo sottoscrivere al server OPC
                    if (cfg.dir == "OPC_TO_WEBOTS") {
                        std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                        opcua_ctx.pending_subscribes.push_back(parts[0]);
                    }
                    
                    // Se è da Webots verso OPC, abilito i sensori se necessario
                    if (cfg.dir == "WEBOTS_TO_OPC" && cfg.param == "SENSOR_VAL") {
                        PositionSensor *sensor = supervisor->getPositionSensor(cfg.target);
                        if (sensor) sensor->enable(timeStep);
                    }
                    std::cout << "[MAPPING] Applicato " << parts[0] << " su " << cfg.target << " (" << cfg.dir << ")" << std::endl;
                }
            } 
            else if (message.rfind("UNMAP:", 0) == 0) {
                std::string nodeId = message.substr(6);
                if (active_mappings.count(nodeId)) {
                    if (active_mappings[nodeId].dir == "OPC_TO_WEBOTS") {
                        std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                        opcua_ctx.pending_unsubscribes.push_back(nodeId);
                    } else if (active_mappings[nodeId].param == "SENSOR_VAL") {
                        PositionSensor *sensor = supervisor->getPositionSensor(active_mappings[nodeId].target);
                        if (sensor) sensor->disable();
                    }
                    active_mappings.erase(nodeId);
                    std::cout << "[MAPPING] Rimosso " << nodeId << std::endl;
                }
            }
            message = supervisor->wwiReceiveText();
        }

        // 2. AGGIORNAMENTO STATO UI
        bool current_connected = opcua_ctx.connected;
        bool current_error = opcua_ctx.connection_error;
        if (current_connected != last_connected) {
            supervisor->wwiSendText(current_connected ? "STATUS:CONNECTED" : "STATUS:DISCONNECTED");
            last_connected = current_connected;
        }
        if (current_error && current_error != last_error) {
            supervisor->wwiSendText("STATUS:ERROR");
            last_error = current_error;
            last_connected = false;
        }
        if (!current_error) last_error = false;

        if (opcua_ctx.tags_ready) {
            std::vector<DiscoveredTag> tags_copy;
            {
                std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
                tags_copy = opcua_ctx.discovered_tags;
                opcua_ctx.tags_ready = false;
            }
            std::stringstream json;
            json << "TAGS:[";
            for (size_t i = 0; i < tags_copy.size(); ++i) {
                json << "{\"name\":\"" << tags_copy[i].name << "\", \"nodeId\":\"" << tags_copy[i].nodeId << "\"}";
                if (i < tags_copy.size() - 1) json << ",";
            }
            json << "]";
            supervisor->wwiSendText(json.str());
        }

        // 3. ESECUZIONE DELLA MAPPATURA (Lettura/Scrittura HARD REAL-TIME)
        
        // Fase A: Leggiamo i valori dall'OPC (in arrivo) e li applichiamo a Webots
        std::unordered_map<std::string, double> in_values;
        {
            std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
            in_values = opcua_ctx.opc_to_webots_values;
        }

        std::unordered_map<std::string, double> out_values;

        for (const auto& pair : active_mappings) {
            const std::string& nodeIdStr = pair.first;
            const MappingConfig& cfg = pair.second;

            if (cfg.dir == "OPC_TO_WEBOTS") {
                // Abbiamo un valore dall'OPC per questo nodo?
                if (in_values.count(nodeIdStr)) {
                    double val = in_values[nodeIdStr];
                    
                    if (cfg.param == "MOTOR_POS") {
                        Motor *motor = supervisor->getMotor(cfg.target);
                        if (motor) motor->setPosition(val);
                    } else if (cfg.param == "MOTOR_VEL") {
                        Motor *motor = supervisor->getMotor(cfg.target);
                        if (motor) {
                            motor->setPosition(INFINITY); // Necessario in Webots per usare il controllo di velocità
                            motor->setVelocity(val);
                        }
                    } else if (cfg.param.find("TRANS_") == 0) {
                        Node *node = supervisor->getFromDef(cfg.target);
                        if (node) {
                            Field *transField = node->getField("translation");
                            if (transField && transField->getType() == Node::SF_VEC3F) {
                                const double *current = transField->getSFVec3f();
                                double next[3] = { current[0], current[1], current[2] };
                                if (cfg.param == "TRANS_X") next[0] = val;
                                else if (cfg.param == "TRANS_Y") next[1] = val;
                                else if (cfg.param == "TRANS_Z") next[2] = val;
                                transField->setSFVec3f(next);
                            }
                        }
                    }
                }
            } 
            else if (cfg.dir == "WEBOTS_TO_OPC") {
                // Dobbiamo leggere da Webots e spingere nella coda per OPC
                if (cfg.param == "SENSOR_VAL") {
                    PositionSensor *sensor = supervisor->getPositionSensor(cfg.target);
                    if (sensor) {
                        out_values[nodeIdStr] = sensor->getValue();
                    }
                } else if (cfg.param.find("TRANS_") == 0) {
                    Node *node = supervisor->getFromDef(cfg.target);
                    if (node) {
                        Field *transField = node->getField("translation");
                        if (transField && transField->getType() == Node::SF_VEC3F) {
                            const double *current = transField->getSFVec3f();
                            if (cfg.param == "TRANS_X") out_values[nodeIdStr] = current[0];
                            else if (cfg.param == "TRANS_Y") out_values[nodeIdStr] = current[1];
                            else if (cfg.param == "TRANS_Z") out_values[nodeIdStr] = current[2];
                        }
                    }
                }
            }
        }

        // Passa i dati in uscita al thread OPC-UA in modo protetto
        if (!out_values.empty()) {
            std::lock_guard<std::mutex> lock(opcua_ctx.mutex);
            for (const auto& out : out_values) {
                opcua_ctx.webots_to_opc_values[out.first] = out.second;
            }
        }
    }

    opcua_ctx.running = false;
    if (opcua_thread.joinable()) {
        opcua_thread.join();
    }

    delete supervisor;
    return 0;
}