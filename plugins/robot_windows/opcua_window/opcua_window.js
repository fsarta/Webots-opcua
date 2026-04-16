import RobotWindow from 'https://cyberbotics.com/wwi/R2025a/RobotWindow.js';

let windowRobot = null;
let discoveredTags = [];
let mappedTags = {}; // Tiene traccia delle mappature attive: nodeId -> config

window.onload = function() {
    console.log("[opcua_window] Initializing RobotWindow...");
    window.robotWindow = new RobotWindow();

    window.robotWindow.receive = function(message, robot) {
        console.log("[opcua_window] RECEIVED MSG: " + message);
        if (message.startsWith("STATUS:")) {
            updateStatus(message.substring(7));
        } else if (message.startsWith("TAGS:")) {
            try {
                discoveredTags = JSON.parse(message.substring(5));
                renderTagsList();
            } catch (e) {
                console.error("Error parsing tags: ", e);
            }
        } else if (message.startsWith("MAPPINGS:")) {
            try {
                mappedTags = JSON.parse(message.substring(9));
                renderTagsList();
            } catch (e) {
                console.error("Error parsing mappings: ", e);
            }
        } else if (message.startsWith("CONFIG_URL:")) {
            document.getElementById("ip-input").value = message.substring(11);
        }
    };

    document.getElementById("connect-btn").addEventListener("click", () => {
        const ip = document.getElementById("ip-input").value;
        console.log("[opcua_window] Clicked CONNECT with IP: " + ip);
        if (ip && window.robotWindow) {
            updateStatus("CONNECTING...");
            console.log("[opcua_window] Sending CONNECT command to C++ controller...");
            window.robotWindow.send("CONNECT:" + ip);
            console.log("[opcua_window] Send called successfully.");
        } else {
            console.error("[opcua_window] Cannot send: invalid IP or window.robotWindow is null.");
        }
    });

    document.getElementById("save-btn").addEventListener("click", () => {
        window.robotWindow.send("SAVE_CONFIG");
        console.log("[opcua_window] Sent SAVE_CONFIG command.");
    });

    document.getElementById("load-btn").addEventListener("click", () => {
        window.robotWindow.send("LOAD_CONFIG");
        console.log("[opcua_window] Sent LOAD_CONFIG command.");
    });

    document.getElementById("status-indicator").textContent = "Disconnected";
    document.getElementById("status-indicator").className = "status disconnected";
    console.log("[opcua_window] robotWindow initialized successfully. Waiting for interactions.");

    // Carica configurazione all'avvio
    setTimeout(() => {
        if (window.robotWindow) window.robotWindow.send("LOAD_CONFIG");
    }, 500);
};

function updateStatus(statusStr) {
    const statusEl = document.getElementById("status-indicator");
    const connectBtn = document.getElementById("connect-btn");
    const disconnectBtn = document.getElementById("disconnect-btn");
    
    if (statusStr === "CONNECTED") {
        statusEl.textContent = "Connected";
        statusEl.className = "status connected";
        connectBtn.style.display = "none";
        disconnectBtn.style.display = "inline-block";
    } else if (statusStr === "DISCONNECTED" || statusStr === "ERROR") {
        statusEl.textContent = statusStr === "ERROR" ? "Connection Error" : "Disconnected";
        statusEl.className = "status disconnected";
        connectBtn.style.display = "inline-block";
        connectBtn.disabled = false;
        disconnectBtn.style.display = "none";
        
        // Se ci disconnettiamo, puliamo la lista dei tag (opzionale, o li lasciamo in grigio)
        if (statusStr === "DISCONNECTED") {
            discoveredTags = [];
            renderTagsList();
        }
    } else if (statusStr === "CONNECTING...") {
        statusEl.textContent = "Connecting...";
        statusEl.className = "status connecting";
        connectBtn.disabled = true;
    }
}

function buildTree(tags) {
    const root = { name: "Root", children: {}, tags: [] };
    
    tags.forEach(tag => {
        const parts = tag.path ? tag.path.split('/') : [tag.name];
        let current = root;
        
        for (let i = 0; i < parts.length - 1; i++) {
            const folderName = parts[i];
            if (!current.children[folderName]) {
                current.children[folderName] = { name: folderName, children: {}, tags: [] };
            }
            current = current.children[folderName];
        }
        
        current.tags.push(tag);
    });
    
    return root;
}

function renderTree(nodeDom, treeNode, isRoot = false) {
    // Ordina cartelle
    const folderKeys = Object.keys(treeNode.children).sort();
    folderKeys.forEach(key => {
        const childNode = treeNode.children[key];
        
        const details = document.createElement("details");
        details.className = "tree-folder";
        if (isRoot || Object.keys(childNode.children).length > 0) details.open = true;
        
        const summary = document.createElement("summary");
        summary.innerHTML = `📁 ${key}`;
        details.appendChild(summary);
        
        renderTree(details, childNode, false);
        nodeDom.appendChild(details);
    });

    // Ordina e rendi le variabili
    treeNode.tags.sort((a,b) => a.name.localeCompare(b.name)).forEach(tag => {
        const isMapped = mappedTags[tag.nodeId] !== undefined;
        
        const div = document.createElement("div");
        div.className = `tag-item ${isMapped ? 'mapped' : ''}`;
        div.id = `tag-${tag.nodeId.replace(/[^a-zA-Z0-9]/g, '-')}`;

        const header = document.createElement("div");
        header.className = "tag-header";
        
        header.innerHTML = `
            <div>
                ${isMapped ? '<span class="tag-status-badge">MAPPED</span>' : ''}
                <span class="tag-name">📄 ${tag.name}</span>
                <span class="tag-node-id">(${tag.nodeId})</span>
            </div>
            <button class="btn-small btn-config">⚙️ Config</button>
        `;
        
        const configPanel = document.createElement("div");
        configPanel.className = "tag-config";
        
        const currentConf = mappedTags[tag.nodeId] || { dir: 'OPC_TO_WEBOTS', target: tag.name, param: 'MOTOR_POS' };

        configPanel.innerHTML = `
            <div class="config-group">
                <label>Direction</label>
                <select class="map-dir">
                    <option value="OPC_TO_WEBOTS" ${currentConf.dir==='OPC_TO_WEBOTS'?'selected':''}>⬇️ Read (OPC ➔ Webots)</option>
                    <option value="WEBOTS_TO_OPC" ${currentConf.dir==='WEBOTS_TO_OPC'?'selected':''}>⬆️ Write (Webots ➔ OPC)</option>
                </select>
            </div>
            <div class="config-group">
                <label>Node DEF Name</label>
                <input type="text" class="map-target" value="${currentConf.target}" placeholder="e.g. motor_1">
            </div>
            <div class="config-group">
                <label>Parameter</label>
                <select class="map-param">
                    <option value="MOTOR_POS" ${currentConf.param==='MOTOR_POS'?'selected':''}>Motor Position</option>
                    <option value="MOTOR_VEL" ${currentConf.param==='MOTOR_VEL'?'selected':''}>Motor Velocity</option>
                    <option value="SENSOR_VAL" ${currentConf.param==='SENSOR_VAL'?'selected':''}>Sensor Value</option>
                    <option value="TRANS_X" ${currentConf.param==='TRANS_X'?'selected':''}>Translation X</option>
                    <option value="TRANS_Y" ${currentConf.param==='TRANS_Y'?'selected':''}>Translation Y</option>
                    <option value="TRANS_Z" ${currentConf.param==='TRANS_Z'?'selected':''}>Translation Z</option>
                </select>
            </div>
            <div class="config-group" style="flex-grow: 0; justify-content: flex-end; flex-direction: row; gap: 5px;">
                ${isMapped ? `<button class="btn-small btn-unmap">Unmap</button>` : ''}
                <button class="btn-small btn-apply">Apply</button>
            </div>
        `;

        // Toggle Config Panel
        const btnConfig = header.querySelector('.btn-config');
        btnConfig.addEventListener("click", (e) => {
            const isVisible = configPanel.style.display === "flex";
            configPanel.style.display = isVisible ? "none" : "flex";
        });

        // Apply Mapping
        const btnApply = configPanel.querySelector('.btn-apply');
        btnApply.addEventListener("click", (e) => {
            const dir = configPanel.querySelector('.map-dir').value;
            const target = configPanel.querySelector('.map-target').value;
            const param = configPanel.querySelector('.map-param').value;
            
            if(!target) { alert("Inserisci il nome del nodo/device Webots!"); return; }

            mappedTags[tag.nodeId] = { dir, target, param };
            const mapCmd = `MAP:${tag.nodeId}|${dir}|${target}|${param}`;
            window.robotWindow.send(mapCmd);
            renderTagsList();
        });

        const btnUnmap = configPanel.querySelector('.btn-unmap');
        if (btnUnmap) {
            btnUnmap.addEventListener("click", (e) => {
                delete mappedTags[tag.nodeId];
                window.robotWindow.send(`UNMAP:${tag.nodeId}`);
                renderTagsList();
            });
        }

        div.appendChild(header);
        div.appendChild(configPanel);
        nodeDom.appendChild(div);
    });
}

function renderTagsList() {
    const container = document.getElementById("tags-list");
    container.innerHTML = "";

    if (discoveredTags.length === 0) {
        container.innerHTML = '<p class="placeholder-text">No variables found in OPC-UA Server.</p>';
        return;
    }

    const treeData = buildTree(discoveredTags);
    const rootDom = document.createElement("div");
    rootDom.className = "tree-root";
    
    renderTree(rootDom, treeData, true);
    container.appendChild(rootDom);
}