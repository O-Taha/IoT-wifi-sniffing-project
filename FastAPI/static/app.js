let scans = {}; // scan_id => networks[]
let activeScanId = null;

// Récupère les données du backend
async function fetchData() {
    try {
        const res = await fetch("/wifi_scans?limit=50");
        if (!res.ok) {
            throw new Error(`Erreur HTTP : ${res.status}`);
        }
        const jsonData = await res.json(); // Récupère l'objet JSON complet
        updateScans(jsonData.data); // Accède à la propriété `data` de l'objet JSON
    } catch (error) {
        console.error("Erreur lors de la récupération des données :", error);
    }
}


function updateScans(data) {
    for (const scan of data) {
        scans[scan.scan_id] = scan.networks;
    }
    renderTabs();
    if (!activeScanId) {
        activeScanId = Object.keys(scans)[0];
    }
    renderActiveTab();
}

function renderTabs() {
    const tabsDiv = document.getElementById("tabs");
    tabsDiv.innerHTML = "";

    const scanIds = Object.keys(scans).sort((a, b) => a - b);

    scanIds.forEach(id => {
        const tab = document.createElement("div");
        tab.className = "tab" + (id == activeScanId ? " active" : "");
        tab.textContent = "Scan " + id;
        tab.onclick = () => {
            activeScanId = id;
            renderTabs();
            renderActiveTab();
        };
        tabsDiv.appendChild(tab);
    });
}

function renderActiveTab() {
    const container = document.getElementById("tab-content");
    container.innerHTML = "";

    const networks = scans[activeScanId] || [];

    // Grouping by group_ssid
    const groups = {};
    for (const net of networks) {
        if (!groups[net.group_ssid]) groups[net.group_ssid] = [];
        groups[net.group_ssid].push(net);
    }

    const table = document.createElement("table");
    table.border = "1";
    table.style.width = "100%";
    table.innerHTML = `
        <tr>
            <th>Group</th>
            <th>SSID</th>
            <th>BSSID</th>
            <th>Channel</th>
            <th>RSSI</th>
        </tr>
    `;

    for (const group in groups) {
        // N'afficher un groupe QUE s'il a plusieurs SSID
        if (groups[group].length <= 1) continue;

        const items = groups[group];

        // Ligne de groupe
        const groupRow = document.createElement("tr");
        groupRow.className = "group-row";
        groupRow.innerHTML = `
            <td colspan="5">${group}</td>
        `;
        table.appendChild(groupRow);

        // Lignes enfants
        items.forEach(net => {
            const row = document.createElement("tr");
            row.className = "child-row";
            row.innerHTML = `
                <td></td>
                <td>${net.ssid}</td>
                <td>${net.bssid}</td>
                <td>${net.channel}</td>
                <td>${net.rssi}</td>
            `;
            table.appendChild(row);
        });
    }

    container.appendChild(table);
}

// Rafraîchissement périodique
setInterval(fetchData, 2000);
fetchData();
