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

    document.getElementById("status-indicator").textContent = "Disconnected";
    document.getElementById("status-indicator").className = "status disconnected";
    console.log("[opcua_window] robotWindow initialized successfully. Waiting for interactions.");
};

function updateStatus(statusStr) {
    const statusEl = document.getElementById("status-indicator");
    const btn = document.getElementById("connect-btn");
    
    if (statusStr === "CONNECTED") {
        statusEl.textContent = "Connected";
        statusEl.className = "status connected";
        btn.disabled = true;
    } else if (statusStr === "DISCONNECTED" || statusStr === "ERROR") {
        statusEl.textContent = statusStr === "ERROR" ? "Connection Error" : "Disconnected";
        statusEl.className = "status disconnected";
        btn.disabled = false;
        // In caso di disconnessione manteniamo le UI ma segniamo lo stato
    } else if (statusStr === "CONNECTING...") {
        statusEl.textContent = "Connecting...";
        statusEl.className = "status connecting";
        btn.disabled = true;
    }
}

function renderTagsList() {
    const container = document.getElementById("tags-list");
    container.innerHTML = "";

    if (discoveredTags.length === 0) {
        container.innerHTML = '<p class="placeholder-text">No variables found in OPC-UA Server.</p>';
        return;
    }

    discoveredTags.forEach(tag => {
        const isMapped = mappedTags[tag.nodeId] !== undefined;
        
        const div = document.createElement("div");
        div.className = `tag-item ${isMapped ? 'mapped' : ''}`;
        div.id = `tag-${tag.nodeId.replace(/[^a-zA-Z0-9]/g, '-')}`;

        // Header (cliccabile per espandere/collassare la config)
        const header = document.createElement("div");
        header.className = "tag-header";
        
        const infoHtml = `
            <div>
                ${isMapped ? '<span class="tag-status-badge">MAPPED</span>' : ''}
                <span class="tag-name">${tag.name}</span>
                <span class="tag-node-id">(${tag.nodeId})</span>
            </div>
            <button class="btn-small">⚙️ Config</button>
        `;
        header.innerHTML = infoHtml;
        
        // Config Panel (Nascosto di default)
        const configPanel = document.createElement("div");
        configPanel.className = "tag-config";
        
        // Recupera valori precedenti se già mappato
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
                <label>Webots Node/Device Name</label>
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
                <button class="btn-small btn-apply">Apply Mapping</button>
            </div>
        `;

        // Logica di espansione
        header.addEventListener("click", () => {
            const isVisible = configPanel.style.display === "flex";
            configPanel.style.display = isVisible ? "none" : "flex";
        });

        // Logica pulsanti Config
        const btnApply = configPanel.querySelector('.btn-apply');
        btnApply.addEventListener("click", (e) => {
            e.stopPropagation(); // Evita il click sull'header
            
            const dir = configPanel.querySelector('.map-dir').value;
            const target = configPanel.querySelector('.map-target').value;
            const param = configPanel.querySelector('.map-param').value;
            
            if(!target) { alert("Inserisci il nome del nodo/device Webots!"); return; }

            // Salva nello stato locale UI
            mappedTags[tag.nodeId] = { dir, target, param };
            
            // Invia al backend C++ nel formato: MAP:nodeId|DIR|TARGET|PARAM
            const mapCmd = `MAP:${tag.nodeId}|${dir}|${target}|${param}`;
            window.robotWindow.send(mapCmd);
            
            // Ricarica la UI per mostrare il badge
            renderTagsList();
        });

        const btnUnmap = configPanel.querySelector('.btn-unmap');
        if (btnUnmap) {
            btnUnmap.addEventListener("click", (e) => {
                e.stopPropagation();
                delete mappedTags[tag.nodeId];
                window.robotWindow.send(`UNMAP:${tag.nodeId}`);
                renderTagsList();
            });
        }

        div.appendChild(header);
        div.appendChild(configPanel);
        container.appendChild(div);
    });
}