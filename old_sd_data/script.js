let map;
let isMapFullscreen = false;
let markers = {};
let deviceData = {};
let lastDataHash = '';
let autoFitDone = false;

const LOG_KEY = 'lora_alert_log';
const alertedDevicesThisCycle = new Set();
const icons = {
  p1: L.icon({ iconUrl: 'images/marker-icon-red.png', shadowUrl: 'images/marker-shadow.png', iconSize: [25, 41], iconAnchor: [12, 41], popupAnchor: [1, -34] }),
  p2: L.icon({ iconUrl: 'images/marker-icon-blue.png', shadowUrl: 'images/marker-shadow.png', iconSize: [25, 41], iconAnchor: [12, 41], popupAnchor: [1, -34] })
};

// ---------------- MAP FUNCTIONS ----------------
function initMap() {
  map = L.map('map', { minZoom: 11, maxZoom: 16 }).setView([12.961, 77.598], 14);
  L.tileLayer('./tiles/{z}/{x}/{y}.png', { maxZoom: 16, minZoom: 11, attribution: 'Offline Map' }).addTo(map);
}

// Fix: Define missing function
function fitMapToAllMarkers() {
  const allMarkers = Object.values(markers);
  if (allMarkers.length === 0) return;
  const group = L.featureGroup(allMarkers);
  map.fitBounds(group.getBounds().pad(0.1));
}

function toggleFullscreen() {
  const mapDiv = document.getElementById('map');
  const mapContainer = document.getElementById('map-container');
  const btn = document.getElementById('fullscreen-btn');
  
  mapDiv.classList.toggle('fullscreen');
  mapContainer.classList.toggle('fullscreen');
  btn.innerHTML = isMapFullscreen 
    ? '<span>🔍</span><span>Fullscreen</span>' 
    : '<span>🔍</span><span>Exit Fullscreen</span>';
  isMapFullscreen = !isMapFullscreen;
  setTimeout(() => map.invalidateSize(), 310);
}

function updateMapMarker(deviceId, data) {
  let lat = parseFloat(data.latitude);
  let lon = parseFloat(data.longitude);

  if (isNaN(lat) || isNaN(lon) || lat === 0 || lon === 0) {
    lat = 12.9238;
    lon = 77.4988;
  }

  const popupContent = `
    <div style="font-family: Inter, sans-serif; min-width: 200px;">
      <h4 style="margin: 0 0 10px 0;">Device ${data.id.toUpperCase()}</h4>
      <div><strong>Temp:</strong> ${data.temperature}°C</div>
      <div><strong>Pressure:</strong> ${data.pressure} hPa</div>
      <div><strong>Altitude:</strong> ${data.altitude} m</div>
      <div><strong>Battery:</strong> ${data.battery}%</div>
      <div><strong>Coords:</strong> ${lat.toFixed(6)}, ${lon.toFixed(6)}</div>
      <div><strong>Alert:</strong> ${data.alert == 1 
        ? '<span style="color:red;font-weight:bold">ACTIVE</span>' 
        : '<span style="color:green;">Normal</span>'}</div>
    </div>
  `;

  if (!markers[deviceId]) {
    markers[deviceId] = L.marker([lat, lon], { icon: icons[deviceId] || icons.p1 })
      .addTo(map).bindPopup(popupContent);
  } else {
    markers[deviceId].setLatLng([lat, lon]).setPopupContent(popupContent);
  }

  if (!autoFitDone && Object.keys(markers).length >= 2) {
    fitMapToAllMarkers();
    autoFitDone = true;
  }
}

// ---------------- DEVICE FUNCTIONS ----------------
function renderCard(deviceId) {
  const data = deviceData[deviceId];
  if (!data) return;

  document.getElementById(`temperature-${deviceId}`).innerHTML = `${data.temperature}<span class="metric-unit">°C</span>`;
  document.getElementById(`pressure-${deviceId}`).innerHTML = `${data.pressure}<span class="metric-unit">hPa</span>`;
  document.getElementById(`altitude-${deviceId}`).innerHTML = `${data.altitude}<span class="metric-unit">m</span>`;
  document.getElementById(`id-${deviceId}`).textContent = data.id.toUpperCase();
  document.getElementById(`battery-${deviceId}`).textContent = `${data.battery}%`;

  const batteryFill = document.getElementById(`battery-fill-${deviceId}`);
  batteryFill.style.width = `${data.battery}%`;
  batteryFill.className = 'battery-fill';
  if (data.battery > 60) batteryFill.classList.add('high');
  else if (data.battery > 30) batteryFill.classList.add('medium');
  else batteryFill.classList.add('low');

  document.getElementById(`alert-${deviceId}`).style.display = data.alert == 1 ? 'flex' : 'none';

  // Glow effect
  const dashboard = document.getElementById(`dashboard-${deviceId}`);
  dashboard.style.boxShadow = 'var(--shadow-xl), 0 0 20px rgba(99, 102, 241, 0.3)';
  dashboard.style.borderLeft = '4px solid var(--primary)';
  clearTimeout(dashboard._glowTimeout);
  dashboard._glowTimeout = setTimeout(() => {
    dashboard.style.boxShadow = 'var(--shadow-xl)';
    dashboard.style.borderLeft = 'none';
  }, 3000);
}

function updateCard(newData) {
  const deviceId = newData.id.toLowerCase();
  deviceData[deviceId] = { ...deviceData[deviceId], ...newData, lastUpdated: new Date().toLocaleString() };
  updateMapMarker(deviceId, deviceData[deviceId]);
  renderCard(deviceId);
}

// ---------------- ALERT FUNCTIONS ----------------
function loadPersistedLog() {
  try { return JSON.parse(localStorage.getItem(LOG_KEY)) || []; }
  catch { return []; }
}

function savePersistedLog(logArray) {
  localStorage.setItem(LOG_KEY, JSON.stringify(logArray));
}

function addLogEntry(deviceId, message) {
  const logArray = loadPersistedLog();
  const ts = new Date().toLocaleString();
  const entry = `${ts}: ${message}`;
  if (logArray[logArray.length - 1] !== entry) {
    logArray.push(entry);
    if (logArray.length > 50) logArray.shift();
    savePersistedLog(logArray);
    displayAlertLog();
  }
}

function displayAlertLog() {
  const logContainer = document.getElementById('alert-log');
  const logEmpty = document.getElementById('log-empty');
  const logArray = loadPersistedLog();
  logContainer.innerHTML = '';
  if (logArray.length === 0) { logEmpty.style.display = 'block'; return; }
  logEmpty.style.display = 'none';
  logArray.slice().reverse().forEach(entry => {
    const li = document.createElement('li');
    li.textContent = entry;
    logContainer.appendChild(li);
  });
}

function processDataForAlerts() {
  fetch('data.txt')
    .then(res => res.text())
    .then(data => {
      data.trim().split('\n').forEach(line => {
        try {
          const d = JSON.parse(line);
          const id = d.id?.toLowerCase();
          if (!['p1', 'p2'].includes(id)) return;
          if (parseInt(d.alert) === 1) {
            if (!alertedDevicesThisCycle.has(id)) {
              alertedDevicesThisCycle.add(id);
              addLogEntry(id, `ALERT detected - Temp: ${d.temperature}°C, Pressure: ${d.pressure}hPa, Battery: ${d.battery}%`);
            }
          } else {
            if (alertedDevicesThisCycle.has(id)) {
              alertedDevicesThisCycle.delete(id);
              addLogEntry(id, `Alert CLEARED`);
            }
          }
        } catch {}
      });
    });
}

// ---------------- FETCH LOOP ----------------
function startFetchLoop() {
  setInterval(() => {
    fetch('latest.txt?_=' + Date.now())
      .then(res => res.text())
      .then(text => {
        const hash = btoa(text);
        if (hash === lastDataHash) return;
        lastDataHash = hash;
        const data = JSON.parse(text);
        if (data?.id && ['p1', 'p2'].includes(data.id.toLowerCase())) {
          updateCard(data);
        }
      })
      .catch(() => {});
  }, 1000);

  setInterval(processDataForAlerts, 5000);
}

// ---------------- INIT ----------------
document.addEventListener('DOMContentLoaded', () => {
  initMap();
  document.getElementById('fullscreen-btn').onclick = toggleFullscreen;
  displayAlertLog();
  createClearButton();

  ['p1', 'p2'].forEach(id => {
    deviceData[id] = { temperature: '--', pressure: '--', altitude: '--', battery: '--', alert: 0, id: id.toUpperCase(), latitude: null, longitude: null };
    renderCard(id);
  });

  startFetchLoop();

  // Fit All button
  const fitBtn = document.createElement('button');
  fitBtn.innerHTML = '<span>🗺️</span><span>Fit All</span>';
  fitBtn.className = 'map-fit-btn';
  fitBtn.onclick = fitMapToAllMarkers;
  document.getElementById('map-container').appendChild(fitBtn);
});

function createClearButton() {
  const header = document.querySelector('.alert-log-header');
  const btn = document.createElement('button');
  btn.id = 'clear-log-btn';
  btn.textContent = 'Clear Log';
  btn.onclick = () => { localStorage.removeItem(LOG_KEY); displayAlertLog(); };
  header.appendChild(btn);
}
