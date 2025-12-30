#!/usr/bin/env python3
"""
LoRa Network Dashboard - Laptop Application
Real-time monitoring, alerts, and cloud sync
Requires: pip install flask folium requests pandas plotly
"""

from flask import Flask, render_template, jsonify, request, send_file
import requests
import json
import time
import threading
from datetime import datetime, timedelta
import pandas as pd
import folium
from pathlib import Path

app = Flask(__name__)

# Configuration
ESP32_IP = "192.168.4.1"  # Change to your ESP32 IP
UBIDOTS_TOKEN = "YOUR_UBIDOTS_TOKEN"
POLL_INTERVAL = 5  # seconds
DATA_FILE = "local_data.csv"
ALERT_FILE = "local_alerts.csv"

# Global data storage
node_data = {}
alerts = []
last_update = None

# Bangalore fallback coordinates
FALLBACK_LAT = 12.9716
FALLBACK_LNG = 77.5946

class DataCollector:
    def __init__(self):
        self.running = False
        self.thread = None
    
    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._collect_loop, daemon=True)
        self.thread.start()
    
    def stop(self):
        self.running = False
    
    def _collect_loop(self):
        """Continuously fetch data from ESP32"""
        while self.running:
            try:
                # Fetch node data
                response = requests.get(f"http://{ESP32_IP}/api/nodes", timeout=5)
                if response.status_code == 200:
                    data = response.json()
                    self._process_data(data)
                
                # Fetch alerts
                response = requests.get(f"http://{ESP32_IP}/api/alerts", timeout=5)
                if response.status_code == 200:
                    self._process_alerts(response.text)
                
            except Exception as e:
                print(f"Error fetching data: {e}")
            
            time.sleep(POLL_INTERVAL)
    
    def _process_data(self, data):
        """Process and store node data"""
        global node_data, last_update
        
        for node in data.get('nodes', []):
            node_id = node['id']
            node_data[node_id] = {
                'temp': node['temp'],
                'pres': node['pres'],
                'alt': node['alt'],
                'bat': node['bat'],
                'lat': node['lat'],
                'lng': node['lng'],
                'rssi': node['rssi'],
                'snr': node['snr'],
                'alert': node['alert'],
                'offline': node['offline'],
                'lastSeen': node['lastSeen'],
                'timestamp': datetime.now()
            }
            
            # Save to CSV
            self._save_to_csv(node_id, node)
        
        last_update = datetime.now()
    
    def _process_alerts(self, alert_text):
        """Process alert history"""
        global alerts
        lines = alert_text.strip().split('\n')
        
        for line in lines[-20:]:  # Keep last 20 alerts
            if line and ',' in line:
                parts = line.split(',')
                if len(parts) >= 3:
                    alerts.append({
                        'timestamp': parts[0],
                        'node': parts[1],
                        'message': ','.join(parts[2:]),
                        'time': datetime.now()
                    })
        
        # Keep only recent alerts
        alerts = alerts[-50:]
    
    def _save_to_csv(self, node_id, data):
        """Save data to local CSV file"""
        try:
            df = pd.DataFrame([{
                'timestamp': datetime.now(),
                'node': node_id,
                'temp': data['temp'],
                'pressure': data['pres'],
                'altitude': data['alt'],
                'battery': data['bat'],
                'latitude': data['lat'],
                'longitude': data['lng'],
                'rssi': data['rssi'],
                'snr': data['snr'],
                'alert': data['alert']
            }])
            
            # Append to file
            if Path(DATA_FILE).exists():
                df.to_csv(DATA_FILE, mode='a', header=False, index=False)
            else:
                df.to_csv(DATA_FILE, mode='w', header=True, index=False)
        except Exception as e:
            print(f"Error saving to CSV: {e}")

# Initialize collector
collector = DataCollector()

@app.route('/')
def index():
    """Main dashboard page"""
    return render_template('dashboard.html')

@app.route('/api/data')
def get_data():
    """API endpoint for current node data"""
    return jsonify({
        'nodes': node_data,
        'lastUpdate': last_update.isoformat() if last_update else None
    })

@app.route('/api/alerts')
def get_alerts():
    """API endpoint for alerts"""
    return jsonify({'alerts': alerts})

@app.route('/api/map')
def get_map():
    """Generate interactive map"""
    # Create map centered on average location
    if node_data:
        lats = [n['lat'] for n in node_data.values()]
        lngs = [n['lng'] for n in node_data.values()]
        center_lat = sum(lats) / len(lats)
        center_lng = sum(lngs) / len(lngs)
    else:
        center_lat, center_lng = FALLBACK_LAT, FALLBACK_LNG
    
    m = folium.Map(location=[center_lat, center_lng], zoom_start=13)
    
    # Add node markers
    for node_id, data in node_data.items():
        color = 'red' if data['offline'] else ('orange' if data['bat'] < 15 else 'green')
        
        folium.Marker(
            [data['lat'], data['lng']],
            popup=f"""
                <b>{node_id}</b><br>
                Temp: {data['temp']:.1f}°C<br>
                Alt: {data['alt']:.0f}m<br>
                Battery: {data['bat']}%<br>
                RSSI: {data['rssi']} dBm
            """,
            icon=folium.Icon(color=color)
        ).add_to(m)
    
    return m._repr_html_()

@app.route('/api/download')
def download_data():
    """Download CSV data"""
    if Path(DATA_FILE).exists():
        return send_file(DATA_FILE, as_attachment=True)
    else:
        return "No data available", 404

@app.route('/api/charts/<node_id>')
def get_charts(node_id):
    """Generate time series charts for a node"""
    try:
        df = pd.read_csv(DATA_FILE)
        df = df[df['node'] == node_id].tail(100)  # Last 100 readings
        
        if df.empty:
            return jsonify({'error': 'No data for this node'}), 404
        
        df['timestamp'] = pd.to_datetime(df['timestamp'])
        
        charts = {
            'temperature': df[['timestamp', 'temp']].to_dict('records'),
            'altitude': df[['timestamp', 'altitude']].to_dict('records'),
            'battery': df[['timestamp', 'battery']].to_dict('records'),
            'pressure': df[['timestamp', 'pressure']].to_dict('records')
        }
        
        return jsonify(charts)
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/api/upload_cloud', methods=['POST'])
def upload_to_cloud():
    """Upload data to Ubidots"""
    if not UBIDOTS_TOKEN or UBIDOTS_TOKEN == "YOUR_UBIDOTS_TOKEN":
        return jsonify({'error': 'Ubidots token not configured'}), 400
    
    try:
        for node_id, data in node_data.items():
            url = f"https://industrial.api.ubidots.com/api/v1.6/devices/{node_id.lower()}"
            headers = {
                'X-Auth-Token': UBIDOTS_TOKEN,
                'Content-Type': 'application/json'
            }
            payload = {
                'temperature': data['temp'],
                'pressure': data['pres'],
                'altitude': data['alt'],
                'battery': data['bat'],
                'latitude': data['lat'],
                'longitude': data['lng'],
                'rssi': data['rssi']
            }
            
            response = requests.post(url, json=payload, headers=headers)
            if response.status_code != 200:
                print(f"Failed to upload {node_id}: {response.text}")
        
        return jsonify({'success': True, 'message': 'Data uploaded to Ubidots'})
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/api/clear_data', methods=['POST'])
def clear_data():
    """Clear local data files"""
    try:
        if Path(DATA_FILE).exists():
            Path(DATA_FILE).unlink()
        if Path(ALERT_FILE).exists():
            Path(ALERT_FILE).unlink()
        return jsonify({'success': True})
    except Exception as e:
        return jsonify({'error': str(e)}), 500

# HTML Template for dashboard
DASHBOARD_HTML = '''
<!DOCTYPE html>
<html>
<head>
    <title>LoRa Network Dashboard</title>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: #fff;
            padding: 20px;
        }
        .container { max-width: 1400px; margin: 0 auto; }
        h1 { 
            text-align: center; 
            margin-bottom: 30px;
            font-size: 2.5em;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
        }
        .controls {
            background: rgba(255,255,255,0.1);
            padding: 15px;
            border-radius: 10px;
            margin-bottom: 20px;
            backdrop-filter: blur(10px);
        }
        .btn {
            background: #00ff88;
            color: #000;
            border: none;
            padding: 10px 20px;
            margin: 5px;
            border-radius: 5px;
            cursor: pointer;
            font-weight: bold;
            transition: all 0.3s;
        }
        .btn:hover { 
            background: #00cc66;
            transform: translateY(-2px);
            box-shadow: 0 5px 15px rgba(0,255,136,0.4);
        }
        .status {
            display: inline-block;
            margin-left: 20px;
            padding: 8px 15px;
            background: rgba(0,0,0,0.3);
            border-radius: 5px;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .card {
            background: rgba(255,255,255,0.95);
            color: #333;
            padding: 20px;
            border-radius: 15px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.3);
            transition: transform 0.3s;
        }
        .card:hover { transform: translateY(-5px); }
        .card h2 {
            color: #667eea;
            margin-bottom: 15px;
            border-bottom: 2px solid #667eea;
            padding-bottom: 10px;
        }
        .metric {
            display: flex;
            justify-content: space-between;
            padding: 8px 0;
            border-bottom: 1px solid #eee;
        }
        .metric:last-child { border-bottom: none; }
        .metric-label { 
            font-weight: 600;
            color: #666;
        }
        .metric-value {
            font-weight: bold;
            color: #333;
        }
        .alert-badge {
            background: #ff4444;
            color: white;
            padding: 3px 8px;
            border-radius: 10px;
            font-size: 0.8em;
            font-weight: bold;
        }
        .offline { opacity: 0.5; }
        .warning { border-left: 4px solid #ffaa00; }
        .danger { border-left: 4px solid #ff4444; }
        .ok { border-left: 4px solid #00ff88; }
        #map-container {
            height: 500px;
            background: white;
            border-radius: 15px;
            overflow: hidden;
            margin-bottom: 30px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.3);
        }
        .alert-list {
            max-height: 400px;
            overflow-y: auto;
            background: rgba(255,255,255,0.95);
            color: #333;
            padding: 20px;
            border-radius: 15px;
        }
        .alert-item {
            padding: 10px;
            margin: 5px 0;
            background: #fff3cd;
            border-left: 4px solid #ff4444;
            border-radius: 5px;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .updating { animation: pulse 1s infinite; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🛰️ LoRa Sensor Network Dashboard</h1>
        
        <div class="controls">
            <button class="btn" onclick="refreshData()">🔄 Refresh</button>
            <button class="btn" onclick="downloadCSV()">📥 Download CSV</button>
            <button class="btn" onclick="uploadCloud()">☁️ Upload to Cloud</button>
            <button class="btn" onclick="clearData()">🗑️ Clear Data</button>
            <span class="status" id="status">Loading...</span>
        </div>

        <div id="map-container"></div>

        <div class="grid" id="nodes-grid"></div>

        <div class="alert-list" id="alerts">
            <h2>Recent Alerts</h2>
            <div id="alert-content"></div>
        </div>
    </div>

    <script>
        function refreshData() {
            document.getElementById('status').classList.add('updating');
            
            fetch('/api/data')
                .then(r => r.json())
                .then(data => {
                    updateNodes(data.nodes);
                    document.getElementById('status').textContent = 
                        '✅ Last update: ' + new Date().toLocaleTimeString();
                })
                .catch(err => {
                    document.getElementById('status').textContent = '❌ Connection error';
                })
                .finally(() => {
                    document.getElementById('status').classList.remove('updating');
                });
            
            fetch('/api/alerts')
                .then(r => r.json())
                .then(data => updateAlerts(data.alerts));
            
            fetch('/api/map')
                .then(r => r.text())
                .then(html => {
                    document.getElementById('map-container').innerHTML = html;
                });
        }

        function updateNodes(nodes) {
            let html = '';
            for (let id in nodes) {
                let node = nodes[id];
                let status = node.offline ? 'danger offline' : 
                            (node.bat < 15 ? 'warning' : 'ok');
                
                html += `
                    <div class="card ${status}">
                        <h2>${id} ${node.offline ? '📵 OFFLINE' : '✅'}</h2>
                        <div class="metric">
                            <span class="metric-label">Temperature</span>
                            <span class="metric-value">${node.temp.toFixed(1)}°C</span>
                        </div>
                        <div class="metric">
                            <span class="metric-label">Pressure</span>
                            <span class="metric-value">${node.pres.toFixed(1)} hPa</span>
                        </div>
                        <div class="metric">
                            <span class="metric-label">Altitude</span>
                            <span class="metric-value">${node.alt.toFixed(0)} m</span>
                        </div>
                        <div class="metric">
                            <span class="metric-label">Battery</span>
                            <span class="metric-value">${node.bat}%</span>
                        </div>
                        <div class="metric">
                            <span class="metric-label">GPS</span>
                            <span class="metric-value">${node.lat.toFixed(4)}, ${node.lng.toFixed(4)}</span>
                        </div>
                        <div class="metric">
                            <span class="metric-label">Signal (RSSI)</span>
                            <span class="metric-value">${node.rssi} dBm</span>
                        </div>
                        <div class="metric">
                            <span class="metric-label">Last Seen</span>
                            <span class="metric-value">${node.lastSeen}s ago</span>
                        </div>
                        ${node.alert ? '<span class="alert-badge">🚨 ALERT</span>' : ''}
                    </div>
                `;
            }
            document.getElementById('nodes-grid').innerHTML = html;
        }

        function updateAlerts(alerts) {
            let html = '';
            alerts.slice(-10).reverse().forEach(alert => {
                html += `
                    <div class="alert-item">
                        <strong>${alert.node}</strong>: ${alert.message}
                        <br><small>${alert.timestamp}</small>
                    </div>
                `;
            });
            document.getElementById('alert-content').innerHTML = html || '<p>No alerts</p>';
        }

        function downloadCSV() {
            window.location.href = '/api/download';
        }

        function uploadCloud() {
            fetch('/api/upload_cloud', {method: 'POST'})
                .then(r => r.json())
                .then(data => {
                    alert(data.message || data.error);
                });
        }

        function clearData() {
            if (confirm('Clear all local data?')) {
                fetch('/api/clear_data', {method: 'POST'})
                    .then(() => {
                        alert('Data cleared');
                        refreshData();
                    });
            }
        }

        // Auto-refresh every 5 seconds
        setInterval(refreshData, 5000);
        refreshData();
    </script>
</body>
</html>
'''

# Create templates directory and save HTML
Path('templates').mkdir(exist_ok=True)
with open('templates/dashboard.html', 'w') as f:
    f.write(DASHBOARD_HTML)

if __name__ == '__main__':
    print("=" * 60)
    print("LoRa Network Dashboard")
    print("=" * 60)
    print(f"ESP32 IP: {ESP32_IP}")
    print(f"Dashboard: http://localhost:5000")
    print("=" * 60)
    
    # Start data collector
    collector.start()
    
    try:
        # Run Flask app
        app.run(host='0.0.0.0', port=5000, debug=False)
    except KeyboardInterrupt:
        print("\nShutting down...")
        collector.stop()
