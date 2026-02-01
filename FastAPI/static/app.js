const apsDiv = document.getElementById("aps");
const mapDiv = document.getElementById("map");

apsDiv.innerHTML = "<h3>Access Points détectés</h3>";

if (!APS || APS.length === 0 || !POSITION) {
    mapDiv.innerHTML = "<p>Aucune position disponible</p>";
    apsDiv.innerHTML += "<p>Aucun AP détecté</p>";
} else {
    APS.forEach(ap => {
        apsDiv.innerHTML += `
            <div>
                <strong>${ap[0]}</strong><br>
                RSSI : ${ap[1]} dBm
                <hr>
            </div>
        `;
    });

    const map = L.map("map").setView(
        [POSITION.latitude, POSITION.longitude],
        18
    );

    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19
    }).addTo(map);

    L.marker([POSITION.latitude, POSITION.longitude])
        .addTo(map)
        .bindPopup("Position estimée")
        .openPopup();
        
}
