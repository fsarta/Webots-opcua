import RobotWindow from 'https://cyberbotics.com/wwi/R2025a/RobotWindow.js';

let discoveredTags = [];
let mappedTags = {}; 
let lastValues = {};

window.onload = function() {
    window.robotWindow = new RobotWindow();

    window.robotWindow.receive = function(message, robot) {
        if (message.startsWith("STATUS:")) {
            updateStatus(message.substring(7));
        } else if (message.startsWith("TAGS:")) {
            try {
                discoveredTags = JSON.parse(message.substring(5));
                renderTagsList();
            } catch (e) { console.error("Error parsing tags", e); }
        } else if (message.startsWith("MAPPINGS:")) {
            try {
                mappedTags = JSON.parse(message.substring(9));
                renderTagsList();
                renderMappingsTable();
            } catch (e) { console.error("Error parsing mappings", e); }
        } else if (message.startsWith("VALUES:")) {
            try {
                lastValues = JSON.parse(message.substring(7));
                updateLiveValues();
            } catch (e) { }
        } else if (message.startsWith("CONFIG_URL:")) {
            const parts = message.substring(11).split('|');
            document.getElementById("ip-input").value = parts[0];
            if (parts.length > 1) document.getElementById("pub-input").value = parts[1];
        }
    };

    document.getElementById("connect-btn").onclick = () => {
        const ip = document.getElementById("ip-input").value;
        const pub = document.getElementById("pub-input").value;
        if (ip && window.robotWindow) {
            updateStatus("CONNECTING...");
            window.robotWindow.send(`CONNECT:${ip}|${pub}`);
        }
    };

    document.getElementById("disconnect-btn").onclick = () => {
        if (window.robotWindow) window.robotWindow.send("DISCONNECT");
    };

    document.getElementById("save-btn").onclick = () => {
        window.robotWindow.send("SAVE_CONFIG");
    };

    document.getElementById("load-btn").onclick = () => {
        window.robotWindow.send("LOAD_CONFIG");
    };

    // Auto-load
    setTimeout(() => { if (window.robotWindow) window.robotWindow.send("LOAD_CONFIG"); }, 500);
};

function updateStatus(statusStr) {
    const statusEl = document.getElementById("status-indicator");
    const connectBtn = document.getElementById("connect-btn");
    const disconnectBtn = document.getElementById("disconnect-btn");
    
    statusEl.textContent = statusStr;
    if (statusStr === "CONNECTED") {
        statusEl.className = "status connected";
        connectBtn.style.display = "none";
        disconnectBtn.style.display = "inline-block";
    } else {
        statusEl.className = "status disconnected";
        connectBtn.style.display = "inline-block";
        connectBtn.disabled = (statusStr === "CONNECTING...");
        disconnectBtn.style.display = "none";
        if (statusStr === "DISCONNECTED") {
            discoveredTags = [];
            renderTagsList();
        }
    }
}

function updateLiveValues() {
    for (const nodeId in lastValues) {
        const safeId = nodeId.replace(/[^a-zA-Z0-9]/g, '-');
        const valSpan = document.getElementById(`val-${safeId}`);
        if (valSpan) {
            const val = lastValues[nodeId];
            valSpan.textContent = typeof val === 'number' ? val.toFixed(3) : val;
        }
    }
}

function renderMappingsTable() {
    const section = document.getElementById("active-mappings-section");
    const tbody = document.getElementById("mappings-table-body");
    tbody.innerHTML = "";
    const keys = Object.keys(mappedTags);
    if (keys.length === 0) { section.style.display = "none"; return; }
    section.style.display = "block";
    keys.forEach(nodeId => {
        const m = mappedTags[nodeId];
        const tr = document.createElement("tr");
        tr.innerHTML = `
            <td title="${nodeId}" style="max-width: 150px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 10px;">${nodeId}</td>
            <td><span class="badge ${m.dir.toLowerCase()}">${m.dir === 'OPC_TO_WEBOTS' ? 'READ' : 'WRITE'}</span></td>
            <td><strong>${m.target}</strong></td>
            <td><code>${m.param}</code></td>
            <td><button class="btn-small btn-unmap">❌</button></td>
        `;
        tr.querySelector('.btn-unmap').onclick = () => {
            window.robotWindow.send(`UNMAP:${nodeId}`);
            delete mappedTags[nodeId];
            renderTagsList();
            renderMappingsTable();
        };
        tbody.appendChild(tr);
    });
}

function buildTree(tags) {
    const root = { name: "Root", children: {}, tags: [] };
    tags.forEach(tag => {
        const parts = tag.path ? tag.path.split('/') : [tag.name];
        if (parts[0] === "") parts.shift();
        let current = root;
        for (let i = 0; i < parts.length - 1; i++) {
            const folderName = parts[i];
            if (!current.children[folderName]) current.children[folderName] = { name: folderName, children: {}, tags: [] };
            current = current.children[folderName];
        }
        current.tags.push(tag);
    });
    return root;
}

function renderTree(nodeDom, treeNode, isRoot = false) {
    const folderKeys = Object.keys(treeNode.children).sort();
    folderKeys.forEach(key => {
        const childNode = treeNode.children[key];
        const details = document.createElement("details");
        details.className = "tree-folder";
        if (isRoot) details.open = true;
        const summary = document.createElement("summary");
        summary.innerHTML = `📁 ${key}`;
        details.appendChild(summary);
        renderTree(details, childNode, false);
        nodeDom.appendChild(details);
    });

    treeNode.tags.sort((a,b) => a.name.localeCompare(b.name)).forEach(tag => {
        const isMapped = mappedTags[tag.nodeId] !== undefined;
        const safeId = tag.nodeId.replace(/[^a-zA-Z0-9]/g, '-');
        const div = document.createElement("div");
        div.className = `tag-item ${isMapped ? 'mapped' : ''}`;
        
        const header = document.createElement("div");
        header.className = "tag-header";
        header.innerHTML = `
            <div style="display: flex; align-items: center; gap: 8px; flex-grow: 1;">
                ${isMapped ? '<span class="tag-status-badge">MAPPED</span>' : ''}
                <span class="tag-name">📄 ${tag.name}</span>
                <span class="live-value" id="val-${safeId}">---</span>
            </div>
            <button class="btn-small btn-config">⚙️</button>
        `;

        const configPanel = document.createElement("div");
        configPanel.className = "tag-config";
        const currentConf = mappedTags[tag.nodeId] || { dir: 'OPC_TO_WEBOTS', target: tag.name, param: 'MOTOR_POS', sampling: 100 };
        configPanel.innerHTML = `
            <div class="config-group"><label>Dir</label><select class="map-dir">
                <option value="OPC_TO_WEBOTS" ${currentConf.dir==='OPC_TO_WEBOTS'?'selected':''}>Read</option>
                <option value="WEBOTS_TO_OPC" ${currentConf.dir==='WEBOTS_TO_OPC'?'selected':''}>Write</option>
            </select></div>
            <div class="config-group"><label>DEF</label><input type="text" class="map-target" value="${currentConf.target}"></div>
            <div class="config-group"><label>Param</label><select class="map-param">
                <option value="MOTOR_POS" ${currentConf.param==='MOTOR_POS'?'selected':''}>Motor Pos</option>
                <option value="MOTOR_VEL" ${currentConf.param==='MOTOR_VEL'?'selected':''}>Motor Vel</option>
                <option value="SENSOR_VAL" ${currentConf.param==='SENSOR_VAL'?'selected':''}>Sensor/Field</option>
                <option value="TRANS_X" ${currentConf.param==='TRANS_X'?'selected':''}>Trans X</option>
                <option value="TRANS_Y" ${currentConf.param==='TRANS_Y'?'selected':''}>Trans Y</option>
                <option value="TRANS_Z" ${currentConf.param==='TRANS_Z'?'selected':''}>Trans Z</option>
            </select></div>
            <div class="config-group"><label>Sampling (ms)</label><input type="number" class="map-sampling" value="${currentConf.sampling || 100}" step="10" min="10"></div>
            <div style="display: flex; align-items: flex-end;"><button class="btn-small btn-apply">Apply</button></div>
        `;

        header.querySelector('.btn-config').onclick = () => {
            configPanel.style.display = (configPanel.style.display === "flex" ? "none" : "flex");
        };
        configPanel.querySelector('.btn-apply').onclick = () => {
            const dir = configPanel.querySelector('.map-dir').value;
            const target = configPanel.querySelector('.map-target').value;
            const param = configPanel.querySelector('.map-param').value;
            const sampling = configPanel.querySelector('.map-sampling').value;
            mappedTags[tag.nodeId] = { dir, target, param, sampling };
            window.robotWindow.send(`MAP:${tag.nodeId}|${dir}|${target}|${param}|${sampling}`);
            renderTagsList();
            renderMappingsTable();
        };

        div.appendChild(header);
        div.appendChild(configPanel);
        nodeDom.appendChild(div);
    });
}

function renderTagsList() {
    const container = document.getElementById("tags-list");
    container.innerHTML = "";
    if (discoveredTags.length === 0) {
        container.innerHTML = '<p class="placeholder-text">Connect to see tags...</p>';
        return;
    }
    const treeData = buildTree(discoveredTags);
    const rootDom = document.createElement("div");
    rootDom.className = "tree-root";
    renderTree(rootDom, treeData, true);
    container.appendChild(rootDom);
}