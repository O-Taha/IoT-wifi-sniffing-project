async function fetchScans() {
    let res = await fetch("/wifi_scans?limit=50");
    let json = await res.json();

    const root = document.getElementById("scans");
    root.innerHTML = "";

    for (let scan of json.data) {
        const div = document.createElement("div");
        div.className = "scan-card";

        div.innerHTML = `
            <div class="scan-header">
                Scan #${scan.scan_id} – ${scan.device_id}
                <br><small>${scan.created_at}</small>
            </div>
            <table>
                <thead>
                    <tr>
                        <th>Group</th>
                        <th>SSID</th>
                        <th>BSSID</th>
                        <th>RSSI</th>
                        <th>CH</th>
                        <th>Enc</th>
                    </tr>
                </thead>
                <tbody>
                    ${scan.networks.map(n => `
                        <tr>
                            <td>${n.group_ssid}</td>
                            <td>${n.ssid}</td>
                            <td>${n.bssid}</td>
                            <td>${formatRssi(n.rssi)}</td>
                            <td>${n.channel}</td>
                            <td>${n.encryption}</td>
                        </tr>
                    `).join("")}
                </tbody>
            </table>
        `;

        root.appendChild(div);
    }
}

function formatRssi(rssi) {
    let cls = "rssi-veryweak";
    if (rssi > -60)      cls = "rssi-strong";
    else if (rssi > -70) cls = "rssi-medium";
    else if (rssi > -80) cls = "rssi-weak";

    return `<span class="badge ${cls}">${rssi} dBm</span>`;
}

fetchScans();

// refresh every 10 sec
setInterval(fetchScans, 10000);

