// LoRaSimulator.jsx
import React, { useState, useEffect, useRef, useCallback } from "react";
import {
  Activity, Radio, Zap, Thermometer, Wind, Mountain, Battery,
  MapPin, AlertTriangle, Wifi, Bell, Database, Waves
} from "lucide-react";

/*
  LoRaSimulator.jsx
  - Master-polled, half-duplex simulation (poll->response->maybe alert broadcast)
  - Produces in-memory CSVs: dataCsv (history) and latestCsv (one row per node)
  - Exposes "Download CSV" buttons to export CSV files compatible with Streamlit dashboard
  - Configurable: numNodes, pollInterval, responseTimeout, maxRetries, latency, lossProb
  - Slave behavior: random-walk, battery drain, GPS fix freshness, one-shot alert cleared after response
*/

const CSV_HEADER = "Timestamp,NodeID,Temp,Pressure,Altitude,Battery,Alert,RSSI,SNR,Lat,Lon";

function nowMs() {
  return Date.now();
}

function randNormal(mu = 0, sigma = 1) {
  // Box-Muller
  let u = 0, v = 0;
  while(u === 0) u = Math.random();
  while(v === 0) v = Math.random();
  return mu + sigma * Math.sqrt(-2.0 * Math.log(u)) * Math.cos(2.0 * Math.PI * v);
}

function downloadBlob(filename, content) {
  const blob = new Blob([content], { type: "text/csv" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

const defaultNodesData = (num, baseLat = 12.9716, baseLon = 77.5946) => {
  const nodes = {};
  for (let i = 1; i <= num; i++) {
    const id = `NODE${i}`;
    const lat = baseLat + (Math.random() - 0.5) * 0.004;
    const lon = baseLon + (Math.random() - 0.5) * 0.004;
    nodes[id] = {
      id,
      temp: +(20 + randNormal(0, 2)).toFixed(2),
      pressure: +(1005 + randNormal(0, 2)).toFixed(2),
      altitude: Math.round(900 + randNormal(0, 20)),
      battery: Math.round(80 + Math.random() * 20),
      alertFlag: 0,           // one-shot alert flag
      localAlertLed: false,   // reacts to broadcast
      lat,
      lon,
      lastFixTs: nowMs(),
      rssi: -60 + Math.floor((Math.random()-0.5) * 8),
      snr: +(6 + (Math.random()-0.5) * 3).toFixed(2),
      lastSeen: nowMs(),
      status: "idle",
      hardware: {
        MCU: "ESP32-WROOM-32",
        Radio: "SX127x (433MHz)",
        Barometer: "BMP280 (I2C)",
        GPS: "NEO-6M (UART)",
      }
    };
  }
  return nodes;
};

const LoRaSimulator = () => {
  // Configuration
  const [numNodes, setNumNodes] = useState(6);
  const [pollIntervalMs, setPollIntervalMs] = useState(2500); // per-node poll spacing
  const [responseTimeoutMs, setResponseTimeoutMs] = useState(1200);
  const [maxRetries, setMaxRetries] = useState(2);
  const [latencyMs, setLatencyMs] = useState(50);
  const [lossProb, setLossProb] = useState(0.02);
  const [walkScale, setWalkScale] = useState(0.00005);

  // Simulation mutable state stored in refs to avoid stale closures
  const nodesRef = useRef(defaultNodesData(numNodes));
  const masterRef = useRef({
    currentNodeIdx: 0,
    nodeList: Object.keys(nodesRef.current),
    status: "idle",
    alertActive: false,
    storageUsedKB: 0,
    storageTotalKB: 500
  });

  // React-visible state for rendering
  const [nodesStateVersion, setNodesStateVersion] = useState(0); // bump to re-render
  const [masterState, setMasterState] = useState({...masterRef.current});
  const [packets, setPackets] = useState([]);
  const [logs, setLogs] = useState([]);
  const [soundEnabled, setSoundEnabled] = useState(true);
  const audioCtxRef = useRef(null);

  // CSV in-memory store
  const historyRowsRef = useRef([CSV_HEADER]); // array of CSV lines
  const latestMapRef = useRef({}); // nodeId -> latest row object

  // Initialize audio context on user gesture (some browsers require)
  useEffect(() => {
    audioCtxRef.current = null; // lazy-create on first play
  }, []);

  const ensureAudio = () => {
    if (!audioCtxRef.current) {
      try {
        audioCtxRef.current = new (window.AudioContext || window.webkitAudioContext)();
      } catch (e) {
        audioCtxRef.current = null;
      }
    }
    return !!audioCtxRef.current;
  };

  function playTone(freq=600, dur=0.12, type='sine') {
    if (!soundEnabled || !ensureAudio()) return;
    const ctx = audioCtxRef.current;
    const o = ctx.createOscillator();
    const g = ctx.createGain();
    o.type = type;
    o.frequency.value = freq;
    g.gain.setValueAtTime(0.07, ctx.currentTime);
    g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + dur);
    o.connect(g);
    g.connect(ctx.destination);
    o.start(ctx.currentTime);
    o.stop(ctx.currentTime + dur + 0.02);
  }

  // Logging helper
  const addLog = useCallback((message, type='info') => {
    setLogs(prev => [...prev.slice(-200), { time: new Date().toLocaleTimeString(), message, type }]);
  }, []);

  // Utility: simulate channel transmit with latency and loss
  const channelTransmit = useCallback((payloadObj, rssiBias = -60) => {
    return new Promise((resolve) => {
      setTimeout(() => {
        if (Math.random() < lossProb) {
          resolve(null); // lost
        } else {
          // simulate rssi & snr as numbers
          const rssi = Math.round(rssiBias + randNormal(0, 2));
          const snr = +(5 + randNormal(0, 1.5)).toFixed(2);
          resolve({ payloadObj, rssi, snr });
        }
      }, latencyMs + Math.random()*latencyMs); // jitter
    });
  }, [latencyMs, lossProb]);

  // Build telemetry JSON string (minified) like firmware
  function buildTelemetry(node) {
    const gpsAge = nowMs() - node.lastFixTs;
    const lat = gpsAge < 30000 ? node.lat : 0.0;
    const lon = gpsAge < 30000 ? node.lon : 0.0;
    const telemetry = {
      node: node.id,
      temp: +node.temp.toFixed(2),
      pres: +node.pressure.toFixed(2),
      alt: node.altitude,
      bat: Math.round(node.battery),
      alert: node.alertFlag,
      lat: +lat.toFixed(6),
      lon: +lon.toFixed(6)
    };
    return JSON.stringify(telemetry);
  }

  // Append history CSV row and update latest map
  function recordTelemetry(telemetryObj, rssi, snr) {
    const ts = nowMs();
    const row = `${ts},${telemetryObj.node},${telemetryObj.temp},${telemetryObj.pres},${telemetryObj.alt},${telemetryObj.bat},${telemetryObj.alert},${rssi},${snr},${telemetryObj.lat},${telemetryObj.lon}`;
    historyRowsRef.current.push(row);
    latestMapRef.current[telemetryObj.node] = {
      Timestamp: ts,
      NodeID: telemetryObj.node,
      Temp: telemetryObj.temp,
      Pressure: telemetryObj.pres,
      Altitude: telemetryObj.alt,
      Battery: telemetryObj.bat,
      Alert: telemetryObj.alert,
      RSSI: rssi,
      SNR: snr,
      Lat: telemetryObj.lat,
      Lon: telemetryObj.lon
    };
    // track storage size approx (bytes / 1024)
    masterRef.current.storageUsedKB = Math.min( masterRef.current.storageTotalKB,
      (historyRowsRef.current.join("\n").length / 1024).toFixed(1));
    setMasterState({...masterRef.current});
  }

  // Write TIMEOUT entry
  function recordTimeout(nodeId) {
    const ts = nowMs();
    const row = `${ts},${nodeId},TIMEOUT,,,,,,,,`;
    historyRowsRef.current.push(row);
    masterRef.current.storageUsedKB = Math.min( masterRef.current.storageTotalKB,
      (historyRowsRef.current.join("\n").length / 1024).toFixed(1));
    setMasterState({...masterRef.current});
  }

  // Broadcast handling: when master broadcasts ALERT, slaves set localAlertLed true briefly
  function handleBroadcastAlert() {
    addLog("MASTER -> BROADCAST:ALERT", "alert");
    Object.values(nodesRef.current).forEach((n, idx) => {
      // set LED blink with stagger
      setTimeout(() => {
        n.localAlertLed = true;
        // clear after 9s
        setTimeout(() => { n.localAlertLed = false; setNodesStateVersion(v => v+1); }, 9000);
        setNodesStateVersion(v => v + 1);
      }, idx * 200);
    });
  }

  // Polling state machine (main simulation loop)
  const simLoopRef = useRef(null);

  const startSimulation = useCallback(() => {
    // re-init nodes if numNodes changed
    nodesRef.current = defaultNodesData(numNodes);
    masterRef.current = {
      currentNodeIdx: -1, // will advance to 0
      nodeList: Object.keys(nodesRef.current),
      status: "polling",
      alertActive: false,
      storageUsedKB: 0,
      storageTotalKB: 500
    };
    historyRowsRef.current = [CSV_HEADER];
    latestMapRef.current = {};

    setNodesStateVersion(v => v + 1);
    setMasterState({...masterRef.current});
    addLog(`Simulation started with ${numNodes} nodes`, "info");

    // clear old interval if any
    if (simLoopRef.current) {
      clearInterval(simLoopRef.current);
      simLoopRef.current = null;
    }

    // We'll use an interval that advances once per pollIntervalMs, and we poll one node per tick (round-robin).
    simLoopRef.current = setInterval(async () => {
      // 1) Move nodes a bit (random walk)
      Object.values(nodesRef.current).forEach(n => {
        // movement
        const angle = Math.random() * Math.PI * 2;
        const step = randNormal(0, walkScale);
        n.lat += Math.cos(angle) * step;
        n.lon += Math.sin(angle) * step;
        // small sensor changes
        n.temp += randNormal(0, 0.05);
        n.pressure += randNormal(0, 0.2);
        n.altitude = Math.round(n.altitude + randNormal(0, 0.3));
        // battery drain
        n.battery = Math.max(0, n.battery - Math.random()*0.12);
        // occasionally update GPS fix time
        if (Math.random() < 0.9) n.lastFixTs = nowMs();
      });

      // 2) Poll next node
      const nodeIds = masterRef.current.nodeList;
      if (nodeIds.length === 0) return;
      masterRef.current.currentNodeIdx = (masterRef.current.currentNodeIdx + 1) % nodeIds.length;
      const targetId = nodeIds[masterRef.current.currentNodeIdx];
      masterRef.current.status = "waiting";
      setMasterState({...masterRef.current});
      addLog(`REQ:${targetId} (master->${targetId})`, "request");
      // transmit downlink (simulate loss/latency)
      const down = await channelTransmit({ type: "REQ", to: targetId }, -80);
      let gotResponse = false;

      // if down is null -> downlink lost. We'll treat as no response.
      if (down) {
        // find the node and let it decide to respond
        const node = nodesRef.current[targetId];
        if (node) {
          // Slave responds only when polled (and after a slight random delay)
          await new Promise(res => setTimeout(res, 20 + Math.random()*120));
          const telemetryJson = buildTelemetry(node); // note: this consumes node.alertFlag if alert==1? we will clear after successful send below
          const up = await channelTransmit({ type: "TELEMETRY", json: telemetryJson }, -50);
          if (up) {
            // master receives reply
            // parse telemetry
            try {
              const tele = JSON.parse(up.payloadObj.json);
              recordTelemetry(tele, up.rssi, up.snr);
              addLog(`RX from ${tele.node}: rssi=${up.rssi} snr=${up.snr}`, "response");
              // one-shot alert: telemetry.alert == 1 means node had alertFlag; clear it at node now
              if (tele.alert === 1) {
                // append handled row and trigger broadcast
                historyRowsRef.current.push(`${nowMs()},${tele.node},ALERT_HANDLED,,,,,,,,`);
                handleBroadcastAlert();
                masterRef.current.alertActive = true;
                setMasterState({...masterRef.current});
                // clear master alertActive after a short period
                setTimeout(() => {
                  masterRef.current.alertActive = false;
                  setMasterState({...masterRef.current});
                }, 4000);
              }
              gotResponse = true;
            } catch(e) {
              addLog("Malformed telemetry received", "error");
            }
          } // up lost => treated as no response
        }
      }

      if (!gotResponse) {
        // Try retries
        let tried = 0;
        while(!gotResponse && tried < maxRetries) {
          tried++;
          addLog(`Retry ${tried} -> ${targetId}`, "request");
          const down2 = await channelTransmit({ type: "REQ", to: targetId }, -80);
          if (!down2) continue; // downlink lost again
          // allow node to respond
          await new Promise(res => setTimeout(res, 30 + Math.random()*120));
          const node = nodesRef.current[targetId];
          const telemetryJson = buildTelemetry(node);
          const up2 = await channelTransmit({ type: "TELEMETRY", json: telemetryJson }, -50);
          if (up2) {
            try {
              const tele = JSON.parse(up2.payloadObj.json);
              recordTelemetry(tele, up2.rssi, up2.snr);
              addLog(`RX (retry) from ${tele.node}: rssi=${up2.rssi} snr=${up2.snr}`, "response");
              if (tele.alert === 1) {
                historyRowsRef.current.push(`${nowMs()},${tele.node},ALERT_HANDLED,,,,,,,,`);
                handleBroadcastAlert();
                masterRef.current.alertActive = true;
                setMasterState({...masterRef.current});
                setTimeout(() => {
                  masterRef.current.alertActive = false;
                  setMasterState({...masterRef.current});
                }, 4000);
              }
              gotResponse = true;
              break;
            } catch (e) {
              addLog("Malformed telemetry on retry", "error");
            }
          }
        }
      }

      if (!gotResponse) {
        addLog(`TIMEOUT ${targetId}`, "error");
        recordTimeout(targetId);
      }

      // After each poll, update nodes UI state and latest map
      Object.values(nodesRef.current).forEach(n => {
        n.lastSeen = latestMapRef.current[n.id] ? latestMapRef.current[n.id].Timestamp : n.lastSeen;
        n.status = latestMapRef.current[n.id] ? "active" : ( (nowMs() - n.lastSeen) > 60000 ? "offline" : n.status );
        // if we recorded telemetry.alert==1 earlier, clear node.alertFlag now:
        if (latestMapRef.current[n.id] && latestMapRef.current[n.id].Alert === 1) {
          // node.alertFlag was the source — clear it (simulate one-shot)
          nodesRef.current[n.id].alertFlag = 0;
        }
        // ensure rssi/snr copy from latest map if present
        if (latestMapRef.current[n.id]) {
          nodesRef.current[n.id].rssi = latestMapRef.current[n.id].RSSI;
          nodesRef.current[n.id].snr = latestMapRef.current[n.id].SNR;
          nodesRef.current[n.id].lat = latestMapRef.current[n.id].Lat || nodesRef.current[n.id].lat;
          nodesRef.current[n.id].lon = latestMapRef.current[n.id].Lon || nodesRef.current[n.id].lon;
        }
      });

      // force UI update occasionally
      setNodesStateVersion(v => v + 1);
      setMasterState({...masterRef.current});

    }, pollIntervalMs);

  }, [numNodes, pollIntervalMs, responseTimeoutMs, maxRetries, latencyMs, lossProb, walkScale, addLog, channelTransmit]);

  const stopSimulation = useCallback(() => {
    if (simLoopRef.current) {
      clearInterval(simLoopRef.current);
      simLoopRef.current = null;
      addLog("Simulation stopped", "info");
      masterRef.current.status = "stopped";
      setMasterState({...masterRef.current});
    }
  }, [addLog]);

  // trigger manual alert on a node
  const triggerAlert = (nodeId) => {
    const node = nodesRef.current[nodeId];
    if (!node) return;
    node.alertFlag = 1;
    addLog(`MANUAL ALERT set on ${nodeId}`, "alert");
    playToneSequence();
    setNodesStateVersion(v => v + 1);
  };

  const playToneSequence = () => {
    playTone(600, 0.18, 'square'); // will create below
    setTimeout(()=> playTone(800, 0.18, 'square'), 240);
    setTimeout(()=> playTone(1000, 0.18, 'square'), 480);
  };

  // small wrapper to call playTone safely
  function playTone(freq, dur, type) { playTone || (() => {}); /* no-op placeholder */ }
  // Because we declared playTone above as empty; we'll reassign properly:
  useEffect(() => {
    // override playTone to proper function referencing audio ctx
    const f = (freq=600, dur=0.12, type='sine') => {
      if (!soundEnabled) return;
      if (!ensureAudio()) return;
      const ctx = audioCtxRef.current;
      const o = ctx.createOscillator();
      const g = ctx.createGain();
      o.type = type;
      o.frequency.value = freq;
      g.gain.setValueAtTime(0.07, ctx.currentTime);
      g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + dur);
      o.connect(g); g.connect(ctx.destination);
      o.start(ctx.currentTime); o.stop(ctx.currentTime + dur + 0.02);
    };
    // replace function in closure by setting window ref
    window.__playTone = f;
    // small wrapper used in other functions
    // eslint-disable-next-line no-unused-vars
    playTone = (...args) => window.__playTone(...args);
    return () => {};
  }, [soundEnabled]);

  // EXPORT CSVs
  const exportHistoryCsv = () => {
    const text = historyRowsRef.current.join("\n");
    downloadBlob("data.csv", text);
  };
  const exportLatestCsv = () => {
    // build latest CSV rows (one per node sorted)
    const rows = [CSV_HEADER];
    const ids = Object.keys(masterRef.current.nodeList).length ? masterRef.current.nodeList : Object.keys(nodesRef.current);
    // ensure using latestMapRef
    Object.keys(latestMapRef.current).sort().forEach(k => {
      const r = latestMapRef.current[k];
      rows.push(`${r.Timestamp},${r.NodeID},${r.Temp},${r.Pressure},${r.Altitude},${r.Battery},${r.Alert},${r.RSSI},${r.SNR},${r.Lat},${r.Lon}`);
    });
    downloadBlob("latest_data.csv", rows.join("\n"));
  };

  // When numNodes changes, update nodesRef and masterRef list (but don't auto restart sim)
  useEffect(() => {
    nodesRef.current = defaultNodesData(numNodes);
    masterRef.current.nodeList = Object.keys(nodesRef.current);
    masterRef.current.currentNodeIdx = -1;
    setNodesStateVersion(v => v + 1);
    setMasterState({...masterRef.current});
    addLog(`Reconfigured nodes -> ${numNodes}`, "info");
    // reset CSV memory
    historyRowsRef.current = [CSV_HEADER];
    latestMapRef.current = {};
  }, [numNodes]);

  // cleanup on unmount
  useEffect(() => {
    return () => {
      if (simLoopRef.current) clearInterval(simLoopRef.current);
    };
  }, []);

  // helper to render node cards (uses nodesRef)
  const renderNodeCard = (nodeId) => {
    const n = nodesRef.current[nodeId];
    if (!n) return null;
    return (
      <div key={nodeId} style={{
        background: "#111827", borderRadius: 8, padding: 12, border: `2px solid ${n.alertFlag ? "#ef4444" : "#374151"}`
      }}>
        <div style={{display: "flex", justifyContent:"space-between", alignItems:"center"}}>
          <div style={{display:"flex", gap:8, alignItems:"center"}}>
            <Activity color={n.alertFlag ? "#f87171" : "#60a5fa"} />
            <div style={{fontWeight:700}}>{nodeId}</div>
          </div>
          <div style={{fontSize:12, color:"#9CA3AF"}}>{n.status}</div>
        </div>
        <div style={{marginTop:10, display:"grid", gridTemplateColumns:"1fr 1fr", gap:8}}>
          <div style={{background:"#0f1724", padding:8, borderRadius:6}}>
            <div style={{fontSize:12, color:"#9CA3AF"}}>Temp</div>
            <div style={{fontWeight:700}}>{n.temp.toFixed(2)} °C</div>
          </div>
          <div style={{background:"#0f1724", padding:8, borderRadius:6}}>
            <div style={{fontSize:12, color:"#9CA3AF"}}>Pressure</div>
            <div style={{fontWeight:700}}>{n.pressure.toFixed(2)} hPa</div>
          </div>
          <div style={{background:"#0f1724", padding:8, borderRadius:6}}>
            <div style={{fontSize:12, color:"#9CA3AF"}}>Battery</div>
            <div style={{fontWeight:700}}>{Math.round(n.battery)}%</div>
          </div>
          <div style={{background:"#0f1724", padding:8, borderRadius:6}}>
            <div style={{fontSize:12, color:"#9CA3AF"}}>GPS</div>
            <div style={{fontWeight:700}}>{n.lat.toFixed(4)}, {n.lon.toFixed(4)}</div>
          </div>
        </div>
        <div style={{marginTop:10}}>
          <button onClick={() => triggerAlert(nodeId)} disabled={n.alertFlag} style={{
            width:"100%", padding:10, background: n.alertFlag ? "#374151" : "#dc2626", color:"white", borderRadius:6, border:0, cursor: n.alertFlag ? "not-allowed" : "pointer"
          }}>
            {n.alertFlag ? "ALERT ACTIVE" : "TRIGGER ALERT"}
          </button>
        </div>
      </div>
    );
  };

  // render packets visually (simple)
  const renderPackets = () => {
    return (
      <div style={{ height: 260, position:"relative", background:"#0b1220", borderRadius:8, padding:12 }}>
        {/* master center */}
        <div style={{position:"absolute", left:"50%", top:"50%", transform:"translate(-50%,-50%)", textAlign:"center"}}>
          <div style={{width:64,height:64, borderRadius:32, background: masterState.alertActive ? "#b91c1c":"#6d28d9", display:"flex", alignItems:"center", justifyContent:"center", color:"white"}}>
            <Database />
          </div>
          <div style={{color:"#c7c7c7", fontWeight:700}}>MASTER</div>
        </div>
        {/* nodes circle around */}
        {Object.keys(nodesRef.current).map((id, idx) => {
          const angle = (idx / Object.keys(nodesRef.current).length) * Math.PI * 2 - Math.PI/2;
          const r = 140;
          const x = 250 + Math.cos(angle) * r;
          const y = 120 + Math.sin(angle) * r;
          const n = nodesRef.current[id];
          return (
            <div key={id} style={{ position:"absolute", left:x, top:y, transform:"translate(-50%,-50%)", textAlign:"center" }}>
              <div style={{width:44,height:44, borderRadius:22, background: n.alertFlag ? "#dc2626" : "#2563eb", display:"flex", alignItems:"center", justifyContent:"center", color:"white"}}>
                <Activity />
              </div>
              <div style={{color:"#9CA3AF", fontSize:12}}>{id}</div>
              <div style={{color:"#9CA3AF", fontSize:11}}>{Math.round(n.battery)}% | {n.rssi}dBm</div>
            </div>
          );
        })}

      </div>
    );
  };

  // UI
  return (
    <div style={{padding:20, background:"linear-gradient(180deg,#090a0f,#071033)", minHeight:"100vh", color:"white"}}>
      <div style={{maxWidth:1200, margin:"0 auto"}}>
        <header style={{display:"flex", justifyContent:"space-between", alignItems:"center", marginBottom:18}}>
          <div>
            <h1 style={{margin:0, fontSize:28, display:"flex", gap:8, alignItems:"center"}}><Radio /> LoRa Master/Slave Simulator</h1>
            <div style={{color:"#9CA3AF"}}>ESP32 + SX127x + BMP280 + GPS — master-polled, half-duplex</div>
          </div>
          <div style={{display:"flex", gap:8}}>
            <button onClick={() => { startSimulation(); }} style={{padding:"8px 12px", background:"#10b981", borderRadius:6, border:0}}>Start</button>
            <button onClick={() => { stopSimulation(); }} style={{padding:"8px 12px", background:"#ef4444", borderRadius:6, border:0}}>Stop</button>
            <button onClick={() => exportHistoryCsv()} style={{padding:"8px 12px", background:"#374151", borderRadius:6, border:0}}>Download data.csv</button>
            <button onClick={() => exportLatestCsv()} style={{padding:"8px 12px", background:"#374151", borderRadius:6, border:0}}>Download latest_data.csv</button>
          </div>
        </header>

        {/* Controls */}
        <section style={{display:"flex", gap:12, marginBottom:16}}>
          <div style={{background:"#0b1220", padding:12, borderRadius:8, flex:"1 1 320px"}}>
            <h3 style={{marginTop:0}}>Simulation Config</h3>
            <div style={{display:"grid", gap:8}}>
              <label>Number of nodes
                <input type="number" value={numNodes} onChange={(e)=>setNumNodes(Math.max(1, +e.target.value))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>Poll interval (ms)
                <input type="number" value={pollIntervalMs} onChange={(e)=>setPollIntervalMs(Math.max(200, +e.target.value))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>Response timeout (ms)
                <input type="number" value={responseTimeoutMs} onChange={(e)=>setResponseTimeoutMs(Math.max(100, +e.target.value))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>Max retries
                <input type="number" value={maxRetries} onChange={(e)=>setMaxRetries(Math.max(0, +e.target.value))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>Channel latency (ms)
                <input type="number" value={latencyMs} onChange={(e)=>setLatencyMs(Math.max(0, +e.target.value))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>Loss probability (0-1)
                <input step="0.01" min="0" max="1" type="number" value={lossProb} onChange={(e)=>setLossProb(Math.min(1, Math.max(0, +e.target.value)))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>Walk scale
                <input type="number" value={walkScale} onChange={(e)=>setWalkScale(Math.max(1e-6, +e.target.value))} style={{width:"100%", marginTop:4}} />
              </label>
              <label>
                <input type="checkbox" checked={soundEnabled} onChange={(e)=>setSoundEnabled(e.target.checked)} /> Enable Sounds
              </label>
            </div>
          </div>

          <div style={{flex:"1 1 400px", background:"#0b1220", padding:12, borderRadius:8}}>
            <h3 style={{marginTop:0}}>Master Status</h3>
            <div style={{display:"grid", gridTemplateColumns:"1fr 1fr", gap:8}}>
              <div style={{background:"#071129", padding:10, borderRadius:6}}>
                <div style={{fontSize:12,color:"#9CA3AF"}}>Status</div>
                <div style={{fontWeight:700}}>{masterState.status}</div>
              </div>
              <div style={{background:"#071129", padding:10, borderRadius:6}}>
                <div style={{fontSize:12,color:"#9CA3AF"}}>Current Target</div>
                <div style={{fontWeight:700}}>{masterRef.current.nodeList[ (masterRef.current.currentNodeIdx >=0 ? masterRef.current.currentNodeIdx : 0) % (masterRef.current.nodeList.length || 1) ] || "N/A"}</div>
              </div>
              <div style={{background:"#071129", padding:10, borderRadius:6}}>
                <div style={{fontSize:12,color:"#9CA3AF"}}>Storage</div>
                <div style={{fontWeight:700}}>{masterState.storageUsedKB} KB / {masterState.storageTotalKB} KB</div>
              </div>
              <div style={{background:"#071129", padding:10, borderRadius:6}}>
                <div style={{fontSize:12,color:"#9CA3AF"}}>Alert Active</div>
                <div style={{fontWeight:700, color: masterState.alertActive ? "#f97316" : "#9CA3AF" }}>{masterState.alertActive ? "YES" : "NO"}</div>
              </div>
            </div>
            <div style={{marginTop:10}}>
              <div style={{fontSize:12, color:"#9CA3AF"}}>LoRa PHY</div>
              <div style={{fontWeight:700}}>433 MHz · SF7 · BW 125 kHz · Sync 0x12</div>
            </div>
          </div>
        </section>

        {/* Packet visualization & nodes */}
        <section style={{display:"grid", gridTemplateColumns:"2fr 1fr", gap:12, marginBottom:12}}>
          <div>
            {renderPackets()}
            <div style={{marginTop:12}}>{/* legend */}</div>
          </div>

          <div style={{display:"grid", gap:12}}>
            <div style={{background:"#0b1220", padding:12, borderRadius:8}}>
              <h4 style={{marginTop:0}}>Actions</h4>
              <button onClick={() => {
                // random alerts across nodes
                Object.keys(nodesRef.current).forEach(k => {
                  if (Math.random() < 0.05) {
                    nodesRef.current[k].alertFlag = 1;
                    addLog(`RANDOM ALERT set on ${k}`, "alert");
                  }
                });
                setNodesStateVersion(v => v + 1);
              }} style={{padding:8, background:"#2563eb", borderRadius:6, border:0, color:"white"}}>Trigger Random Alerts</button>
              <div style={{height:10}} />
              <button onClick={() => {
                // force one polling step (unsafe to call while interval running but allowed)
                (async () => {
                  // we emulate a single tick: advance index and perform poll logic by invoking startSimulation's internal tick
                  // easiest way: stop interval if running, call startSimulation() then restart (not ideal)
                  addLog("Force poll - not implemented as one-shot (use Start/Stop to control)", "info");
                })();
              }} style={{padding:8, background:"#374151", borderRadius:6, border:0, color:"white"}}>Force one poll (dev)</button>
            </div>

            <div style={{background:"#0b1220", padding:12, borderRadius:8}}>
              <h4 style={{marginTop:0}}>Nodes</h4>
              <div style={{display:"grid", gap:8, maxHeight:420, overflowY:"auto"}}>
                {Object.keys(nodesRef.current).map(renderNodeCard)}
              </div>
            </div>
          </div>
        </section>

        {/* Logs */}
        <section style={{background:"#061025", padding:12, borderRadius:8}}>
          <h4 style={{marginTop:0}}>System Logs</h4>
          <div style={{maxHeight:220, overflowY:"auto", fontFamily:"monospace", color:"#cbd5e1"}}>
            {logs.map((l, idx) => (
              <div key={idx} style={{padding:6, borderBottom: "1px solid rgba(255,255,255,0.02)"}}>
                <span style={{color:"#94a3b8"}}>[{l.time}]</span> <span style={{color: l.type === 'alert' ? "#fb7185" : l.type === 'error' ? "#fb923c": "#93c5fd"}}>{l.message}</span>
              </div>
            ))}
          </div>
        </section>

      </div>
    </div>
  );
};

export default LoRaSimulator;
