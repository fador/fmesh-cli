/**
 * fmesh-cli Web Dashboard & OpenStreetMap Visualization
 * Real-time SPA communicating with fmesh-cli REST & Server-Sent Events API
 */

(function() {
  'use strict';

  // --- State Management ---
  const state = {
    devices: [],
    activeDeviceId: '',
    nodes: new Map(),           // node_num -> Node object
    channels: [],
    selectedTarget: { kind: 'channel', target: 0, title: '#Primary' },
    unreadCount: 0,
    selectedNodeNumForMap: null,
    selectedNodeNumForTelemetry: null,
    historyHours: 24,
    showLinks: true,
    showPacketAnim: true,
    showTrails: true,
    showLabels: true,
    darkMap: false,
    charts: {},
    nodeMarkers: new Map(),     // node_num -> Leaflet Marker
    nodeTrails: new Map(),      // node_num -> Leaflet Polyline
    rfLinks: [],                // array of Leaflet Polylines
    rawPackets: [],
    packetCount: 0,
    eventSource: null
  };

  // --- DOM Elements ---
  const el = {
    deviceSelect: document.getElementById('device-select'),
    liveIndicator: document.getElementById('live-indicator'),
    liveStatusText: document.getElementById('live-status-text'),
    navTabs: document.getElementById('nav-tabs'),
    nodesCountBadge: document.getElementById('nodes-count-badge'),
    unreadCountBadge: document.getElementById('unread-count-badge'),
    
    // Map
    mapContainer: document.getElementById('map'),
    btnRecenterMap: document.getElementById('btn-recenter-map'),
    layerToggleLinks: document.getElementById('layer-toggle-links'),
    layerTogglePacketAnim: document.getElementById('layer-toggle-packet-anim'),
    layerToggleTrails: document.getElementById('layer-toggle-trails'),
    layerToggleNames: document.getElementById('layer-toggle-names'),
    layerToggleDarkMap: document.getElementById('layer-toggle-dark-map'),
    trailTimePills: document.getElementById('trail-time-pills'),
    mapNodeDrawer: document.getElementById('map-node-drawer'),
    btnCloseDrawer: document.getElementById('btn-close-drawer'),
    drawerNodeDetails: document.getElementById('drawer-node-details'),
    packetActivityHud: document.getElementById('packet-activity-hud'),
    packetHudList: document.getElementById('packet-hud-list'),
    packetHudCount: document.getElementById('packet-hud-count'),
    btnClearPacketHud: document.getElementById('btn-clear-packet-hud'),


    // Nodes
    nodeSearch: document.getElementById('node-search'),
    nodeSort: document.getElementById('node-sort'),
    nodesGrid: document.getElementById('nodes-grid'),

    // Messages
    channelList: document.getElementById('channel-list'),
    dmList: document.getElementById('dm-list'),
    chatTitle: document.getElementById('chat-title'),
    chatSubtitle: document.getElementById('chat-subtitle'),
    chatMessagesContainer: document.getElementById('chat-messages-container'),
    messageInput: document.getElementById('message-input'),
    btnSendMessage: document.getElementById('btn-send-message'),
    btnRefreshMessages: document.getElementById('btn-refresh-messages'),

    // Telemetry
    telemetryNodeSelect: document.getElementById('telemetry-node-select'),
    metricsSummaryCards: document.getElementById('metrics-summary-cards'),

    // Diagnostics
    tracerouteTargetSelect: document.getElementById('traceroute-target-select'),
    btnRunTraceroute: document.getElementById('btn-run-traceroute'),
    tracerouteResultContainer: document.getElementById('traceroute-result-container'),
    rawPacketStream: document.getElementById('raw-packet-stream'),
    btnClearRaw: document.getElementById('btn-clear-raw'),

    // Config
    configDevicesList: document.getElementById('config-devices-list'),
    configDeviceTitle: document.getElementById('config-device-title'),
    configSearch: document.getElementById('config-search'),
    configTableBody: document.getElementById('config-table-body')
  };

  // --- Leaflet Map Instance ---
  let map = null;
  let tileLayer = null;
  let layerGroupNodes = null;
  let layerGroupLinks = null;
  let layerGroupTrails = null;
  let layerGroupPacketAnim = null;

  // --- Tile Layers ---
  const osmTileUrl = 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png';
  const darkTileUrl = 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png';

  // ==========================================================================
  // Initialization
  // ==========================================================================
  async function init() {
    initNavigation();
    initMap();
    initEventListeners();
    await fetchStatus();
    await fetchDevices();
    await fetchNodes();
    await fetchChannels();
    await fetchLocationHistory();
    await loadMessages();
    await fetchRecentPackets();
    initTelemetryCharts();
    initSSE();
  }

  // ==========================================================================
  // Navigation Tabs
  // ==========================================================================
  function initNavigation() {
    el.navTabs.addEventListener('click', (e) => {
      const tabBtn = e.target.closest('.nav-tab');
      if (!tabBtn) return;

      const targetTab = tabBtn.dataset.tab;
      document.querySelectorAll('.nav-tab').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));

      tabBtn.classList.add('active');
      const pane = document.getElementById(`tab-${targetTab}`);
      if (pane) pane.classList.add('active');

      if (targetTab === 'map' && map) {
        setTimeout(() => map.invalidateSize(), 200);
      }
      if (targetTab === 'messages') {
        state.unreadCount = 0;
        el.unreadCountBadge.style.display = 'none';
        scrollToChatBottom();
      }
      if (targetTab === 'config') {
        loadConfig();
      }
    });
  }

  // ==========================================================================
  // OpenStreetMap Setup
  // ==========================================================================
  function initMap() {
    if (!window.L) {
      console.warn('Leaflet not loaded yet.');
      return;
    }

    // Default center (Helsinki coordinates or fallback)
    map = L.map('map', {
      zoomControl: true,
      attributionControl: false
    }).setView([60.1699, 24.9384], 11);

    tileLayer = L.tileLayer(osmTileUrl, {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }).addTo(map);

    L.control.attribution({ position: 'bottomright' }).addTo(map);

    layerGroupLinks = L.layerGroup().addTo(map);
    layerGroupTrails = L.layerGroup().addTo(map);
    layerGroupPacketAnim = L.layerGroup().addTo(map);
    layerGroupNodes = L.layerGroup().addTo(map);
  }


  function updateMapTiles(useDark) {
    if (!map || !tileLayer) return;
    map.removeLayer(tileLayer);
    tileLayer = L.tileLayer(useDark ? darkTileUrl : osmTileUrl, {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap &copy; CARTO'
    }).addTo(map);
  }

  // ==========================================================================
  // Map Markers, Links, and GPS Trails
  // ==========================================================================
  function createNodeMarkerIcon(node) {
    const shortName = node.short_name || (node.node_id ? node.node_id.slice(-4) : '????');
    const battery = node.battery_level != null ? node.battery_level : '?';
    
    // Status color
    const now = Math.floor(Date.now() / 1000);
    const lastHeard = node.last_heard || 0;
    const diffMin = (now - lastHeard) / 60;
    
    let color = '#10b981'; // green (<15 min)
    if (diffMin > 120) color = '#64748b'; // grey (>2 hours)
    else if (diffMin > 15) color = '#f59e0b'; // amber

    const html = `
      <div class="custom-node-marker">
        <div class="marker-pin" style="background: ${color};"></div>
        <span class="marker-avatar">${escapeHtml(shortName)}</span>
        ${state.showLabels ? `<span class="marker-label">${escapeHtml(node.long_name || shortName)} (${battery}%)</span>` : ''}
      </div>
    `;

    return L.divIcon({
      className: 'node-leaflet-div-icon',
      html: html,
      iconSize: [32, 32],
      iconAnchor: [16, 16],
      popupAnchor: [0, -20]
    });
  }

  function renderMapNodes() {
    if (!map) return;
    layerGroupNodes.clearLayers();
    state.nodeMarkers.clear();

    const bounds = [];

    state.nodes.forEach(node => {
      if (node.latitude != null && node.longitude != null && node.latitude !== 0 && node.longitude !== 0) {
        const latLng = [node.latitude, node.longitude];
        bounds.push(latLng);

        const icon = createNodeMarkerIcon(node);
        const marker = L.marker(latLng, { icon: icon }).addTo(layerGroupNodes);

        marker.on('click', () => {
          selectNodeForMap(node);
        });

        const batteryText = node.battery_level != null ? `${node.battery_level}%` : 'Unknown';
        const snrText = node.snr != null ? `${node.snr.toFixed(1)} dB` : 'N/A';

        marker.bindPopup(`
          <div style="font-family: var(--font-sans); min-width: 180px;">
            <h4 style="margin:0 0 4px; font-weight:700;">${escapeHtml(node.long_name || node.short_name)}</h4>
            <div style="font-family:var(--font-mono); font-size:0.8rem; color:var(--accent-cyan); margin-bottom:8px;">${node.node_id}</div>
            <div style="font-size:0.8rem; line-height:1.4;">
              <div><strong>Battery:</strong> ${batteryText}</div>
              <div><strong>SNR:</strong> ${snrText}</div>
              <div><strong>Role:</strong> ${escapeHtml(node.role || 'Client')}</div>
            </div>
            <button onclick="window.meshApp.openDM(${node.node_num}, '${escapeHtml(node.short_name)}')" class="btn btn-sm btn-primary" style="margin-top:8px; width:100%; justify-content:center;">Direct Message</button>
          </div>
        `);

        state.nodeMarkers.set(node.node_num, marker);
      }
    });

    if (bounds.length > 0 && !state.selectedNodeNumForMap) {
      try {
        map.fitBounds(bounds, { padding: [40, 40], maxZoom: 14 });
      } catch (_) {}
    }

    renderRfLinks();
  }

  function renderRfLinks() {
    if (!map || !layerGroupLinks) return;
    layerGroupLinks.clearLayers();
    state.rfLinks = [];

    if (!state.showLinks) return;

    // Find local radio node as hub
    let hubNode = null;
    state.nodes.forEach(n => {
      if (n.hops_away === 0 && n.latitude != null && n.longitude != null) {
        hubNode = n;
      }
    });

    if (!hubNode) {
      // Pick first node with valid position as reference
      for (const n of state.nodes.values()) {
        if (n.latitude != null && n.longitude != null) {
          hubNode = n;
          break;
        }
      }
    }

    if (!hubNode) return;

    state.nodes.forEach(node => {
      if (node.node_num !== hubNode.node_num && node.latitude != null && node.longitude != null) {
        const snr = node.snr != null ? node.snr : 0;
        let color = '#10b981'; // green
        if (snr < -5) color = '#f43f5e'; // red
        else if (snr < 5) color = '#f59e0b'; // amber

        const latLngs = [
          [hubNode.latitude, hubNode.longitude],
          [node.latitude, node.longitude]
        ];

        const line = L.polyline(latLngs, {
          color: color,
          weight: Math.max(1.5, Math.min(4, 2 + (snr / 10))),
          opacity: 0.6,
          dashArray: node.hops_away && node.hops_away > 1 ? '5, 8' : undefined
        }).addTo(layerGroupLinks);

        line.bindTooltip(`SNR: ${snr.toFixed(1)} dB | Hops: ${node.hops_away ?? 1}`);
        state.rfLinks.push(line);
      }
    });
  }

  async function fetchLocationHistory() {
    if (!state.showTrails) {
      if (layerGroupTrails) layerGroupTrails.clearLayers();
      return;
    }

    const sinceTs = state.historyHours > 0 ? Math.floor(Date.now() / 1000) - (state.historyHours * 3600) : 0;
    try {
      const res = await fetch(`/api/locations?since=${sinceTs}&limit=1000`);
      if (!res.ok) return;
      const rows = await res.json();
      renderLocationTrails(rows);
    } catch (err) {
      console.error('Error fetching locations:', err);
    }
  }

  function renderLocationTrails(rows) {
    if (!map || !layerGroupTrails) return;
    layerGroupTrails.clearLayers();
    state.nodeTrails.clear();

    if (!state.showTrails || !rows || rows.length === 0) return;

    // Group locations by node_num
    const byNode = new Map();
    rows.forEach(r => {
      if (r.latitude !== 0 && r.longitude !== 0) {
        if (!byNode.has(r.node_num)) byNode.set(r.node_num, []);
        byNode.get(r.node_num).push([r.latitude, r.longitude]);
      }
    });

    const colors = ['#06b6d4', '#10b981', '#f59e0b', '#8b5cf6', '#ec4899', '#3b82f6'];
    let idx = 0;

    byNode.forEach((points, nodeNum) => {
      if (points.length < 2) return;
      const color = colors[idx++ % colors.length];
      const poly = L.polyline(points, {
        color: color,
        weight: 3,
        opacity: 0.8
      }).addTo(layerGroupTrails);

      const node = state.nodes.get(nodeNum);
      const name = node ? (node.long_name || node.short_name) : `!${nodeNum.toString(16)}`;
      poly.bindTooltip(`Track: ${escapeHtml(name)} (${points.length} points)`);
      state.nodeTrails.set(nodeNum, poly);
    });
  }

  function selectNodeForMap(node) {
    state.selectedNodeNumForMap = node.node_num;
    el.mapNodeDrawer.classList.remove('collapsed');

    const battery = node.battery_level != null ? `${node.battery_level}%` : 'N/A';
    const voltage = node.voltage != null ? `${node.voltage.toFixed(2)}V` : 'N/A';
    const snr = node.snr != null ? `${node.snr.toFixed(1)} dB` : 'N/A';
    const hops = node.hops_away != null ? node.hops_away : 'Direct';
    const temp = node.temperature != null ? `${node.temperature.toFixed(1)} °C` : 'N/A';
    const humidity = node.relative_humidity != null ? `${node.relative_humidity.toFixed(1)} %` : 'N/A';
    const pressure = node.barometric_pressure != null ? `${node.barometric_pressure.toFixed(1)} hPa` : 'N/A';
    const channelUtil = node.channel_util != null ? `${node.channel_util.toFixed(1)} %` : 'N/A';

    el.drawerNodeDetails.innerHTML = `
      <div class="node-drawer-card">
        <h3>${escapeHtml(node.long_name || node.short_name)}</h3>
        <div class="node-drawer-id">${node.node_id}</div>

        <div class="node-detail-stats">
          <div class="stat-box">
            <span class="stat-label">Battery</span>
            <span class="stat-value">${battery}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">Voltage</span>
            <span class="stat-value">${voltage}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">SNR</span>
            <span class="stat-value">${snr}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">Hops Away</span>
            <span class="stat-value">${hops}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">Temperature</span>
            <span class="stat-value">${temp}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">Humidity</span>
            <span class="stat-value">${humidity}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">Pressure</span>
            <span class="stat-value">${pressure}</span>
          </div>
          <div class="stat-box">
            <span class="stat-label">Channel Util</span>
            <span class="stat-value">${channelUtil}</span>
          </div>
        </div>

        <div class="drawer-actions">
          <button class="btn btn-primary" onclick="window.meshApp.openDM(${node.node_num}, '${escapeHtml(node.short_name)}')">
            Chat / Direct Message
          </button>
          <button class="btn btn-secondary" onclick="window.meshApp.runTraceToNode(${node.node_num})">
            Run Traceroute
          </button>
          <button class="btn btn-secondary" onclick="window.meshApp.inspectTelemetry(${node.node_num})">
            View Telemetry Graphs
          </button>
        </div>
      </div>
    `;
  }

  // ==========================================================================
  // Nodes Explorer Grid
  // ==========================================================================
  function renderNodesGrid() {
    const query = el.nodeSearch.value.toLowerCase().trim();
    const sortKey = el.nodeSort.value;
    const activeFilter = document.querySelector('.filter-chip.active')?.dataset.filter || 'all';

    let list = Array.from(state.nodes.values());

    // Filter
    if (query) {
      list = list.filter(n =>
        (n.long_name && n.long_name.toLowerCase().includes(query)) ||
        (n.short_name && n.short_name.toLowerCase().includes(query)) ||
        (n.node_id && n.node_id.toLowerCase().includes(query)) ||
        (n.hw_model && n.hw_model.toLowerCase().includes(query)) ||
        (n.role && n.role.toLowerCase().includes(query))
      );
    }

    const now = Math.floor(Date.now() / 1000);
    if (activeFilter === 'gps') {
      list = list.filter(n => n.latitude != null && n.longitude != null && n.latitude !== 0);
    } else if (activeFilter === 'recent') {
      list = list.filter(n => (now - (n.last_heard || 0)) < 3600);
    } else if (activeFilter === 'favorites') {
      list = list.filter(n => n.is_favorite);
    } else if (activeFilter === 'routers') {
      list = list.filter(n => n.role && (n.role.includes('ROUTER') || n.role.includes('REPEATER')));
    }

    // Sort
    list.sort((a, b) => {
      if (sortKey === 'name') return (a.long_name || a.short_name).localeCompare(b.long_name || b.short_name);
      if (sortKey === 'battery') return (b.battery_level || 0) - (a.battery_level || 0);
      if (sortKey === 'hops') return (a.hops_away || 99) - (b.hops_away || 99);
      if (sortKey === 'snr') return (b.snr || -99) - (a.snr || -99);
      return (b.last_heard || 0) - (a.last_heard || 0);
    });

    el.nodesCountBadge.textContent = list.length;

    el.nodesGrid.innerHTML = list.map(n => {
      const battery = n.battery_level != null ? `${n.battery_level}%` : 'N/A';
      const snr = n.snr != null ? `${n.snr.toFixed(1)} dB` : 'N/A';
      const hops = n.hops_away != null ? n.hops_away : 0;
      const lastSeenAgo = formatTimeAgo(n.last_heard);

      return `
        <div class="node-card">
          <div class="node-card-header">
            <div class="node-name-block">
              <h3>${escapeHtml(n.long_name || n.short_name)}</h3>
              <div class="node-subtext">
                <span class="node-id-chip">${n.node_id}</span>
                <span>•</span>
                <span>${escapeHtml(n.hw_model || 'Unknown HW')}</span>
              </div>
            </div>
            <span class="node-badge">${escapeHtml(n.role || 'Client')}</span>
          </div>

          <div class="node-metrics-bar">
            <div class="node-metric">
              <span class="node-metric-val">${battery}</span>
              <span class="node-metric-lbl">Battery</span>
            </div>
            <div class="node-metric">
              <span class="node-metric-val">${snr}</span>
              <span class="node-metric-lbl">SNR</span>
            </div>
            <div class="node-metric">
              <span class="node-metric-val">${hops}</span>
              <span class="node-metric-lbl">Hops</span>
            </div>
          </div>

          <div style="font-size: 0.75rem; color: var(--text-muted); margin-bottom: 0.75rem;">
            Last heard: ${lastSeenAgo}
          </div>

          <div class="node-card-actions">
            <button class="btn btn-sm btn-primary" onclick="window.meshApp.openDM(${n.node_num}, '${escapeHtml(n.short_name)}')">Message</button>
            <button class="btn btn-sm btn-secondary" onclick="window.meshApp.focusOnMap(${n.node_num})">Show on Map</button>
          </div>
        </div>
      `;
    }).join('');
  }

  // ==========================================================================
  // Messaging & Chat
  // ==========================================================================
  async function fetchChannels() {
    try {
      const res = await fetch(`/api/channels?device=${encodeURIComponent(state.activeDeviceId)}`);
      if (!res.ok) return;
      state.channels = await res.json();
      renderChannelList();
    } catch (err) {
      console.error('Error fetching channels:', err);
    }
  }

  function renderChannelList() {
    if (!el.channelList) return;
    el.channelList.innerHTML = state.channels.map(ch => {
      const isActive = state.selectedTarget.kind === 'channel' && state.selectedTarget.target === ch.index;
      return `
        <li class="chat-target-item ${isActive ? 'active' : ''}" onclick="window.meshApp.switchTarget('channel', ${ch.index}, '${escapeHtml(ch.name || 'Channel ' + ch.index)}')">
          <span>#${escapeHtml(ch.name || 'ch' + ch.index)}</span>
          <span class="node-badge">${ch.role}</span>
        </li>
      `;
    }).join('');
  }

  async function loadMessages() {
    try {
      const { kind, target } = state.selectedTarget;
      const res = await fetch(`/api/messages?device=${encodeURIComponent(state.activeDeviceId)}&kind=${kind}&target=${target}&limit=100`);
      if (!res.ok) return;
      const messages = await res.json();
      renderMessages(messages);
    } catch (err) {
      console.error('Error loading messages:', err);
    }
  }

  function renderMessages(messages) {
    if (!el.chatMessagesContainer) return;
    el.chatMessagesContainer.innerHTML = messages.map(m => {
      const isOut = m.direction === 'out';
      const senderNode = state.nodes.get(m.from_node);
      const senderName = senderNode ? (senderNode.short_name || senderNode.long_name) : `!${(m.from_node).toString(16)}`;
      const timeStr = new Date(m.ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });

      let ackBadge = '';
      if (isOut && m.ack_state) {
        const color = m.ack_state === 'acked' ? 'var(--accent-emerald)' : m.ack_state === 'pending' ? 'var(--accent-amber)' : 'var(--accent-rose)';
        ackBadge = `<span style="font-size:0.65rem; color:${color}; margin-left:4px;">[${m.ack_state}]</span>`;
      }

      return `
        <div class="chat-message-row ${isOut ? 'outgoing' : 'incoming'}">
          <div class="chat-message-meta">
            <span class="chat-sender-nick">${escapeHtml(isOut ? 'You' : senderName)}</span>
            <span>${timeStr}</span>
            ${ackBadge}
          </div>
          <div class="chat-message-bubble">
            ${escapeHtml(m.text)}
          </div>
        </div>
      `;
    }).join('');

    scrollToChatBottom();
  }

  async function sendMessage() {
    const text = el.messageInput.value.trim();
    if (!text) return;

    el.btnSendMessage.disabled = true;
    try {
      const payload = {
        device: state.activeDeviceId,
        text: text,
        want_ack: true
      };

      if (state.selectedTarget.kind === 'channel') {
        payload.channel_idx = state.selectedTarget.target;
        payload.to_node = 0xFFFFFFFF; // Broadcast
      } else {
        payload.channel_idx = 0;
        payload.to_node = state.selectedTarget.target;
      }

      const res = await fetch('/api/messages', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      if (res.ok) {
        el.messageInput.value = '';
        await loadMessages();
      } else {
        alert('Failed to transmit message over radio.');
      }
    } catch (err) {
      console.error('Error sending message:', err);
    } finally {
      el.btnSendMessage.disabled = false;
      el.messageInput.focus();
    }
  }

  function scrollToChatBottom() {
    if (el.chatMessagesContainer) {
      el.chatMessagesContainer.scrollTop = el.chatMessagesContainer.scrollHeight;
    }
  }

  // ==========================================================================
  // Telemetry Dashboard & Charts
  // ==========================================================================
  function initTelemetryCharts() {
    if (!window.Chart) return;

    const chartOptions = {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      scales: {
        x: { grid: { color: 'rgba(255,255,255,0.05)' }, ticks: { color: '#64748b' } },
        y: { grid: { color: 'rgba(255,255,255,0.05)' }, ticks: { color: '#64748b' } }
      },
      plugins: { legend: { labels: { color: '#94a3b8' } } }
    };

    // 1. Battery & Voltage Chart
    const ctxBattery = document.getElementById('chart-battery')?.getContext('2d');
    if (ctxBattery) {
      state.charts.battery = new Chart(ctxBattery, {
        type: 'line',
        data: {
          labels: ['-50m', '-40m', '-30m', '-20m', '-10m', 'Now'],
          datasets: [
            { label: 'Battery (%)', data: [98, 97, 97, 96, 95, 95], borderColor: '#10b981', backgroundColor: 'rgba(16,185,129,0.1)', tension: 0.3 },
            { label: 'Voltage (V)', data: [4.15, 4.14, 4.13, 4.12, 4.11, 4.10], borderColor: '#06b6d4', yAxisID: 'y1', tension: 0.3 }
          ]
        },
        options: {
          ...chartOptions,
          scales: {
            ...chartOptions.scales,
            y: { ...chartOptions.scales.y, min: 0, max: 100 },
            y1: { position: 'right', min: 3.0, max: 4.5, grid: { drawOnChartArea: false }, ticks: { color: '#06b6d4' } }
          }
        }
      });
    }

    // 2. Environment Chart (Temp & Humidity)
    const ctxEnv = document.getElementById('chart-environment')?.getContext('2d');
    if (ctxEnv) {
      state.charts.environment = new Chart(ctxEnv, {
        type: 'line',
        data: {
          labels: ['-50m', '-40m', '-30m', '-20m', '-10m', 'Now'],
          datasets: [
            { label: 'Temperature (°C)', data: [21.4, 21.6, 21.8, 22.0, 22.1, 22.3], borderColor: '#f59e0b', backgroundColor: 'rgba(245,158,11,0.1)', tension: 0.3 },
            { label: 'Humidity (%)', data: [45, 46, 46, 47, 48, 48], borderColor: '#3b82f6', tension: 0.3 }
          ]
        },
        options: chartOptions
      });
    }

    // 3. Pressure Chart
    const ctxPressure = document.getElementById('chart-pressure')?.getContext('2d');
    if (ctxPressure) {
      state.charts.pressure = new Chart(ctxPressure, {
        type: 'line',
        data: {
          labels: ['-50m', '-40m', '-30m', '-20m', '-10m', 'Now'],
          datasets: [
            { label: 'Pressure (hPa)', data: [1013.2, 1013.3, 1013.1, 1013.4, 1013.2, 1013.5], borderColor: '#8b5cf6', tension: 0.3 }
          ]
        },
        options: chartOptions
      });
    }

    // 4. Channel Utilization
    const ctxUtil = document.getElementById('chart-utilization')?.getContext('2d');
    if (ctxUtil) {
      state.charts.utilization = new Chart(ctxUtil, {
        type: 'bar',
        data: {
          labels: ['Ch 0', 'Ch 1', 'Ch 2'],
          datasets: [
            { label: 'Channel Util (%)', data: [12.4, 3.2, 0.8], backgroundColor: '#06b6d4' },
            { label: 'Air Time TX (%)', data: [2.1, 0.5, 0.1], backgroundColor: '#10b981' }
          ]
        },
        options: chartOptions
      });
    }

    updateTelemetrySummaryCards();
  }

  function updateTelemetrySummaryCards() {
    if (!el.metricsSummaryCards) return;

    let targetNode = null;
    if (state.selectedNodeNumForTelemetry) {
      targetNode = state.nodes.get(state.selectedNodeNumForTelemetry);
    }
    if (!targetNode && state.nodes.size > 0) {
      targetNode = Array.from(state.nodes.values())[0];
    }

    if (!targetNode) {
      el.metricsSummaryCards.innerHTML = '<div class="empty-state">No nodes available for telemetry.</div>';
      return;
    }

    const battery = targetNode.battery_level != null ? `${targetNode.battery_level}%` : 'N/A';
    const temp = targetNode.temperature != null ? `${targetNode.temperature.toFixed(1)}°C` : 'N/A';
    const humidity = targetNode.relative_humidity != null ? `${targetNode.relative_humidity.toFixed(1)}%` : 'N/A';
    const pressure = targetNode.barometric_pressure != null ? `${targetNode.barometric_pressure.toFixed(1)} hPa` : 'N/A';
    const chUtil = targetNode.channel_util != null ? `${targetNode.channel_util.toFixed(1)}%` : 'N/A';

    el.metricsSummaryCards.innerHTML = `
      <div class="metric-kpi-card">
        <div class="kpi-title">Battery Level</div>
        <div class="kpi-value" style="color: #10b981;">${battery}</div>
      </div>
      <div class="metric-kpi-card">
        <div class="kpi-title">Temperature</div>
        <div class="kpi-value" style="color: #f59e0b;">${temp}</div>
      </div>
      <div class="metric-kpi-card">
        <div class="kpi-title">Relative Humidity</div>
        <div class="kpi-value" style="color: #3b82f6;">${humidity}</div>
      </div>
      <div class="metric-kpi-card">
        <div class="kpi-title">Barometric Pressure</div>
        <div class="kpi-value" style="color: #8b5cf6;">${pressure}</div>
      </div>
      <div class="metric-kpi-card">
        <div class="kpi-title">Channel Utilization</div>
        <div class="kpi-value" style="color: #06b6d4;">${chUtil}</div>
      </div>
    `;
  }

  // ==========================================================================
  // Diagnostics & Traceroute
  // ==========================================================================
  async function runTraceroute() {
    const targetNode = parseInt(el.tracerouteTargetSelect.value, 10);
    if (!targetNode) return;

    el.btnRunTraceroute.disabled = true;
    el.tracerouteResultContainer.innerHTML = '<div class="empty-state">Transmitting traceroute request packet over mesh...</div>';

    try {
      const res = await fetch('/api/traceroute', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device: state.activeDeviceId,
          to_node: targetNode
        })
      });

      if (!res.ok) {
        el.tracerouteResultContainer.innerHTML = '<div class="empty-state" style="color:var(--accent-rose)">Failed to initiate traceroute.</div>';
      }
    } catch (err) {
      console.error('Traceroute error:', err);
    } finally {
      el.btnRunTraceroute.disabled = false;
    }
  }

  function handleTracerouteReceived(ev) {
    if (!el.tracerouteResultContainer) return;

    const routeHops = ev.route_ids || [];
    const snrList = ev.snr_towards || [];

    let hopsHtml = routeHops.map((hopId, i) => {
      const snr = snrList[i] != null ? `${snrList[i].toFixed(1)} dB` : '';
      return `
        <div style="display: flex; align-items: center; gap: 0.5rem; margin-bottom: 0.5rem;">
          <span class="badge" style="background:var(--accent-cyan); color:var(--bg-base); font-weight:700;">Hop ${i + 1}</span>
          <span style="font-family:var(--font-mono); font-weight:600;">${escapeHtml(hopId)}</span>
          ${snr ? `<span style="font-size:0.8rem; color:var(--text-muted);">(SNR: ${snr})</span>` : ''}
        </div>
      `;
    }).join('');

    el.tracerouteResultContainer.innerHTML = `
      <div>
        <h4 style="color:var(--accent-emerald); margin-bottom:0.5rem;">✓ Route Resolved</h4>
        <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:1rem;">Target: ${ev.to_id}</div>
        ${hopsHtml || '<div class="empty-state">Direct 1-hop link (no intermediate repeaters).</div>'}
      </div>
    `;
  }

  function handleRawPacket(pkt) {
    if (!el.rawPacketStream) return;

    state.rawPackets.unshift(pkt);
    if (state.rawPackets.length > 50) state.rawPackets.pop();

    const timeStr = new Date().toLocaleTimeString();
    const entry = document.createElement('div');
    entry.className = 'raw-packet-entry';
    entry.innerHTML = `
      <div class="raw-packet-header">
        <span>${escapeHtml(pkt.summary || 'RAW_PACKET')}</span>
        <span>${timeStr}</span>
      </div>
      <div class="raw-packet-hex">${escapeHtml(pkt.hex)}</div>
    `;

    el.rawPacketStream.insertBefore(entry, el.rawPacketStream.firstChild);
    if (el.rawPacketStream.childNodes.length > 50) {
      el.rawPacketStream.removeChild(el.rawPacketStream.lastChild);
    }
  }

  // ==========================================================================
  // Radio Configuration View
  // ==========================================================================
  async function loadConfig() {
    try {
      const res = await fetch(`/api/config?device=${encodeURIComponent(state.activeDeviceId)}`);
      if (!res.ok) return;
      const data = await res.json();
      renderConfigTable(data.config || []);
    } catch (err) {
      console.error('Error loading config:', err);
    }
  }

  function renderConfigTable(lines) {
    if (!el.configTableBody) return;
    const search = el.configSearch.value.toLowerCase().trim();

    el.configTableBody.innerHTML = lines
      .filter(line => !search || line.toLowerCase().includes(search))
      .map(line => {
        const parts = line.split('=');
        const key = parts[0]?.trim() || '';
        const val = parts.slice(1).join('=').trim();

        return `
          <tr>
            <td class="config-key">${escapeHtml(key)}</td>
            <td>
              <input type="text" class="config-val-input" id="cfg-val-${escapeHtml(key)}" value="${escapeHtml(val)}">
            </td>
            <td>
              <button class="btn btn-sm btn-primary" onclick="window.meshApp.saveConfigKey('${escapeHtml(key)}')">Save</button>
            </td>
          </tr>
        `;
      }).join('');
  }

  async function saveConfigKey(key) {
    const input = document.getElementById(`cfg-val-${key}`);
    if (!input) return;
    const value = input.value.trim();

    try {
      const res = await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device: state.activeDeviceId,
          key: key,
          value: value
        })
      });

      if (res.ok) {
        alert(`Setting "${key}" updated successfully.`);
      } else {
        alert(`Failed to update setting "${key}".`);
      }
    } catch (err) {
      console.error('Error updating config:', err);
    }
  }

  // ==========================================================================
  // Server-Sent Events (SSE) Live Feed
  // ==========================================================================
  function initSSE() {
    if (state.eventSource) {
      state.eventSource.close();
    }

    state.eventSource = new EventSource('/api/events');

    state.eventSource.onopen = () => {
      el.liveIndicator.style.display = 'flex';
      el.liveStatusText.textContent = 'LIVE';
    };

    state.eventSource.onerror = () => {
      el.liveStatusText.textContent = 'RECONNECTING';
    };

    state.eventSource.addEventListener('node_updated', (e) => {
      try {
        const data = JSON.parse(e.data);
        if (data.node) {
          state.nodes.set(data.node.node_num, data.node);
          renderMapNodes();
          renderNodesGrid();
          updateTelemetrySummaryCards();
        }
      } catch (_) {}
    });

    state.eventSource.addEventListener('position_received', (e) => {
      try {
        const data = JSON.parse(e.data);
        const node = state.nodes.get(data.from_node);
        if (node) {
          node.latitude = data.latitude;
          node.longitude = data.longitude;
          node.altitude = data.altitude;
          renderMapNodes();
        }
      } catch (_) {}
    });

    state.eventSource.addEventListener('message_received', (e) => {
      try {
        const data = JSON.parse(e.data);
        loadMessages();
        
        // If not in messages tab, increment unread badge
        const isMessagesTab = document.querySelector('.nav-tab[data-tab="messages"]').classList.contains('active');
        if (!isMessagesTab) {
          state.unreadCount++;
          el.unreadCountBadge.textContent = state.unreadCount;
          el.unreadCountBadge.style.display = 'inline-block';
        }
      } catch (_) {}
    });

    state.eventSource.addEventListener('ack_received', () => {
      loadMessages();
    });

    state.eventSource.addEventListener('traceroute_received', (e) => {
      try {
        const data = JSON.parse(e.data);
        handleTracerouteReceived(data);
      } catch (_) {}
    });

    state.eventSource.addEventListener('raw_packet', (e) => {
      try {
        const data = JSON.parse(e.data);
        handleRawPacket(data);
      } catch (_) {}
    });
  }

  // ==========================================================================
  // Fetch APIs
  // ==========================================================================
  async function fetchStatus() {
    try {
      const res = await fetch('/api/status');
      if (!res.ok) return;
      const data = await res.json();
      state.activeDeviceId = data.active_device || '';
    } catch (err) {
      console.error('Error fetching status:', err);
    }
  }

  async function fetchDevices() {
    try {
      const res = await fetch('/api/devices');
      if (!res.ok) return;
      state.devices = await res.json();

      el.deviceSelect.innerHTML = state.devices.map(d => `
        <option value="${escapeHtml(d.id)}" ${d.id === state.activeDeviceId ? 'selected' : ''}>
          ${escapeHtml(d.display_name || d.id)}
        </option>
      `).join('');

      el.configDevicesList.innerHTML = state.devices.map(d => `
        <li class="config-device-item ${d.id === state.activeDeviceId ? 'active' : ''}" onclick="window.meshApp.selectDevice('${escapeHtml(d.id)}')">
          ${escapeHtml(d.display_name || d.id)}
        </li>
      `).join('');
    } catch (err) {
      console.error('Error fetching devices:', err);
    }
  }

  async function fetchNodes() {
    try {
      const res = await fetch('/api/nodes');
      if (!res.ok) return;
      const list = await res.json();

      state.nodes.clear();
      list.forEach(n => state.nodes.set(n.node_num, n));

      renderMapNodes();
      renderNodesGrid();
      updateNodePickers();
    } catch (err) {
      console.error('Error fetching nodes:', err);
    }
  }

  function updateNodePickers() {
    const options = Array.from(state.nodes.values()).map(n => `
      <option value="${n.node_num}">${escapeHtml(n.long_name || n.short_name)} (${n.node_id})</option>
    `).join('');

    if (el.telemetryNodeSelect) el.telemetryNodeSelect.innerHTML = options;
    if (el.tracerouteTargetSelect) el.tracerouteTargetSelect.innerHTML = options;
  }

  // ==========================================================================
  // Event Listeners
  // ==========================================================================
  function initEventListeners() {
    // Device select
    el.deviceSelect?.addEventListener('change', async () => {
      state.activeDeviceId = el.deviceSelect.value;
      await fetch('/api/devices/select', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device: state.activeDeviceId })
      });
      await fetchNodes();
      await fetchChannels();
      await loadMessages();
    });

    // Map controls
    el.btnRecenterMap?.addEventListener('click', () => {
      const bounds = [];
      state.nodes.forEach(n => {
        if (n.latitude && n.longitude) bounds.push([n.latitude, n.longitude]);
      });
      if (bounds.length > 0 && map) {
        map.fitBounds(bounds, { padding: [40, 40], maxZoom: 14 });
      }
    });

    el.layerToggleLinks?.addEventListener('change', () => {
      state.showLinks = el.layerToggleLinks.checked;
      renderRfLinks();
    });

    el.layerTogglePacketAnim?.addEventListener('change', () => {
      state.showPacketAnim = el.layerTogglePacketAnim.checked;
      if (!state.showPacketAnim && layerGroupPacketAnim) {
        layerGroupPacketAnim.clearLayers();
      }
    });

    el.btnClearPacketHud?.addEventListener('click', () => {
      if (el.packetHudList) {
        el.packetHudList.innerHTML = '<div class="packet-hud-empty">Packet history cleared. Waiting for traffic...</div>';
      }
      state.packetCount = 0;
      if (el.packetHudCount) el.packetHudCount.textContent = '0 pkts';
    });

    el.layerToggleTrails?.addEventListener('change', () => {
      state.showTrails = el.layerToggleTrails.checked;
      fetchLocationHistory();
    });

    el.layerToggleNames?.addEventListener('change', () => {
      state.showLabels = el.layerToggleNames.checked;
      renderMapNodes();
    });

    el.layerToggleDarkMap?.addEventListener('change', () => {
      state.darkMap = el.layerToggleDarkMap.checked;
      updateMapTiles(state.darkMap);
    });

    el.trailTimePills?.addEventListener('click', (e) => {
      const pill = e.target.closest('.pill');
      if (!pill) return;
      document.querySelectorAll('#trail-time-pills .pill').forEach(p => p.classList.remove('active'));
      pill.classList.add('active');
      state.historyHours = parseInt(pill.dataset.hours, 10);
      fetchLocationHistory();
    });

    el.btnCloseDrawer?.addEventListener('click', () => {
      el.mapNodeDrawer.classList.add('collapsed');
      state.selectedNodeNumForMap = null;
    });

    // Nodes search & sort
    el.nodeSearch?.addEventListener('input', renderNodesGrid);
    el.nodeSort?.addEventListener('change', renderNodesGrid);
    document.querySelectorAll('.filter-chip').forEach(chip => {
      chip.addEventListener('click', () => {
        document.querySelectorAll('.filter-chip').forEach(c => c.classList.remove('active'));
        chip.classList.add('active');
        renderNodesGrid();
      });
    });

    // Messages
    el.btnSendMessage?.addEventListener('click', sendMessage);
    el.messageInput?.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') sendMessage();
    });
    el.btnRefreshMessages?.addEventListener('click', loadMessages);

    // Diagnostics
    el.btnRunTraceroute?.addEventListener('click', runTraceroute);
    el.btnClearRaw?.addEventListener('click', () => {
      if (el.rawPacketStream) el.rawPacketStream.innerHTML = '';
      state.rawPackets = [];
    });

    // Config search
    el.configSearch?.addEventListener('input', loadConfig);
  }

  // ==========================================================================
  // Live Packet Activity HUD & Map Transmission Visuals
  // ==========================================================================

  async function fetchRecentPackets() {
    try {
      const res = await fetch('/api/packets?limit=30');
      if (!res.ok) return;
      const packets = await res.json();
      if (!Array.isArray(packets)) return;
      packets.forEach(p => {
        addPacketToHud(p, false);
      });
    } catch (err) {
      console.warn('Error fetching recent packets:', err);
    }
  }

  function addPacketToHud(packet, isNew = true) {
    if (!el.packetHudList) return;

    const emptyEl = el.packetHudList.querySelector('.packet-hud-empty');
    if (emptyEl) emptyEl.remove();

    state.packetCount++;
    if (el.packetHudCount) el.packetHudCount.textContent = `${state.packetCount} pkts`;

    const fromNode = state.nodes.get(packet.from_node);
    const toNode = (packet.to_node && packet.to_node !== 0xFFFFFFFF) ? state.nodes.get(packet.to_node) : null;

    const fromName = fromNode ? (fromNode.short_name || fromNode.long_name) : (packet.from_id || `!${packet.from_node.toString(16)}`);
    const toName = packet.broadcast ? 'All (Broadcast)' : (toNode ? (toNode.short_name || toNode.long_name) : (packet.to_id || `!${packet.to_node.toString(16)}`));

    let portBadgeClass = 'port-badge-text';
    let portText = 'TXT';
    if (packet.port_name === 'POSITION_APP') {
      portBadgeClass = 'port-badge-pos';
      portText = 'POS';
    } else if (packet.port_name === 'TRACEROUTE_APP') {
      portBadgeClass = 'port-badge-trace';
      portText = 'TRACE';
    } else if (packet.port_name === 'ROUTING_APP') {
      portBadgeClass = 'port-badge-ack';
      portText = 'ACK';
    } else if (packet.port_name === 'NODEINFO_APP' || packet.port_name === 'TELEMETRY_APP') {
      portBadgeClass = 'port-badge-info';
      portText = packet.port_name === 'NODEINFO_APP' ? 'INFO' : 'TLM';
    }

    const snrText = packet.rx_snr ? `${packet.rx_snr > 0 ? '+' : ''}${packet.rx_snr.toFixed(1)}dB` : '';
    const timeText = formatTimeAgo(packet.ts || Math.floor(Date.now() / 1000));

    const item = document.createElement('div');
    item.className = 'packet-hud-item';
    if (!isNew) item.style.animation = 'none';

    item.title = `${packet.port_name}: ${packet.summary || ''}\nFrom: ${fromName} (${packet.from_id})\nTo: ${toName} (${packet.to_id})\nClick to focus on map`;

    item.innerHTML = `
      <div class="packet-hud-left">
        <span class="hud-port-badge ${portBadgeClass}">${portText}</span>
        <div class="hud-nodes-flow">
          <span class="hud-node-pill">${escapeHtml(fromName)}</span>
          <span class="hud-arrow">&rarr;</span>
          <span class="hud-node-pill">${escapeHtml(toName)}</span>
        </div>
      </div>
      <div class="packet-hud-right">
        ${snrText ? `<span class="hud-snr-badge">${snrText}</span>` : ''}
        <span class="hud-time">${timeText}</span>
      </div>
    `;

    item.addEventListener('click', () => {
      focusPacketOnMap(packet);
    });

    el.packetHudList.prepend(item);

    while (el.packetHudList.children.length > 50) {
      el.packetHudList.lastElementChild.remove();
    }
  }

  function focusPacketOnMap(packet) {
    if (!map) return;
    document.querySelector('.nav-tab[data-tab="map"]')?.click();

    const fromNode = state.nodes.get(packet.from_node);
    const toNode = (packet.to_node && packet.to_node !== 0xFFFFFFFF) ? state.nodes.get(packet.to_node) : null;

    const coords = [];
    if (fromNode && fromNode.latitude && fromNode.longitude) {
      coords.push([fromNode.latitude, fromNode.longitude]);
    }
    if (toNode && toNode.latitude && toNode.longitude) {
      coords.push([toNode.latitude, toNode.longitude]);
    }

    if (coords.length === 2) {
      map.fitBounds(L.latLngBounds(coords), { padding: [80, 80], maxZoom: 15 });
    } else if (coords.length === 1) {
      map.setView(coords[0], 14);
    }
    animatePacketTransmission(packet);
  }

  function animatePacketTransmission(packet) {
    if (!map || !layerGroupPacketAnim || !state.showPacketAnim) return;

    const fromNode = state.nodes.get(packet.from_node);
    const toNode = (packet.to_node && packet.to_node !== 0xFFFFFFFF) ? state.nodes.get(packet.to_node) : null;

    // Flash transmitter marker pin
    if (state.nodeMarkers.has(packet.from_node)) {
      const marker = state.nodeMarkers.get(packet.from_node);
      const markerEl = marker.getElement();
      if (markerEl) {
        markerEl.classList.remove('node-tx-pulse');
        void markerEl.offsetWidth;
        markerEl.classList.add('node-tx-pulse');
        setTimeout(() => markerEl.classList.remove('node-tx-pulse'), 3600);
      }
    }

    // Flash receiver marker pin if unicast
    if (toNode && state.nodeMarkers.has(packet.to_node)) {
      const marker = state.nodeMarkers.get(packet.to_node);
      const markerEl = marker.getElement();
      if (markerEl) {
        markerEl.classList.remove('node-rx-pulse');
        void markerEl.offsetWidth;
        markerEl.classList.add('node-rx-pulse');
        setTimeout(() => markerEl.classList.remove('node-rx-pulse'), 3600);
      }
    }

    // 1. Broadcast packet: Expanding radio ripple wave
    if (packet.broadcast || packet.to_node === 0xFFFFFFFF || packet.port_name === 'POSITION_APP') {
      if (fromNode && fromNode.latitude && fromNode.longitude) {
        createRadioRipple([fromNode.latitude, fromNode.longitude], '#06b6d4');
      }
      return;
    }

    // 2. Traceroute multi-hop chain
    if (packet.port_name === 'TRACEROUTE_APP' && Array.isArray(packet.route) && packet.route.length > 0) {
      const hopsChain = [packet.from_node, ...packet.route];
      if (packet.to_node && packet.to_node !== hopsChain[hopsChain.length - 1]) {
        hopsChain.push(packet.to_node);
      }
      for (let i = 0; i < hopsChain.length - 1; i++) {
        const u = hopsChain[i];
        const v = hopsChain[i + 1];
        const uNode = state.nodes.get(u);
        const vNode = state.nodes.get(v);
        if (uNode && vNode && uNode.latitude && uNode.longitude && vNode.latitude && vNode.longitude) {
          setTimeout(() => {
            createBeamLine([uNode.latitude, uNode.longitude], [vNode.latitude, vNode.longitude], '#a855f7', `Hop ${i + 1}`);
          }, i * 600);
        }
      }
      return;
    }

    // 3. Direct / Unicast transmission beam
    if (fromNode && toNode && fromNode.latitude && fromNode.longitude && toNode.latitude && toNode.longitude) {
      let beamColor = '#06b6d4';
      if (packet.port_name === 'ROUTING_APP') beamColor = '#f59e0b';
      else if (packet.port_name === 'TRACEROUTE_APP') beamColor = '#a855f7';
      
      const tooltipText = packet.port_name === 'ROUTING_APP' ? 'ACK' : (packet.rx_snr ? `${packet.rx_snr.toFixed(1)} dB` : 'Direct Packet');
      createBeamLine([fromNode.latitude, fromNode.longitude], [toNode.latitude, toNode.longitude], beamColor, tooltipText);
    } else if (fromNode && fromNode.latitude && fromNode.longitude) {
      // Fallback: ripple if destination position unknown
      createRadioRipple([fromNode.latitude, fromNode.longitude], '#f59e0b');
    }
  }

  function createRadioRipple(latLng, color = '#06b6d4') {
    if (!map || !layerGroupPacketAnim) return;

    const circle = L.circle(latLng, {
      radius: 400,
      color: color,
      weight: 2,
      fillColor: color,
      fillOpacity: 0.35,
      className: 'radio-ripple-wave'
    }).addTo(layerGroupPacketAnim);

    let start = performance.now();
    const duration = 2200;
    const maxRadius = 3500;

    function animate(now) {
      const elapsed = now - start;
      const progress = Math.min(elapsed / duration, 1);
      const curRadius = 400 + progress * (maxRadius - 400);
      circle.setRadius(curRadius);
      circle.setStyle({
        opacity: 0.8 * (1 - progress),
        fillOpacity: 0.35 * (1 - progress)
      });

      if (progress < 1) {
        requestAnimationFrame(animate);
      } else {
        layerGroupPacketAnim.removeLayer(circle);
      }
    }
    requestAnimationFrame(animate);
  }

  function createBeamLine(latLng1, latLng2, color = '#06b6d4', tooltipText = '') {
    if (!map || !layerGroupPacketAnim) return;

    const line = L.polyline([latLng1, latLng2], {
      color: color,
      weight: 4,
      opacity: 0.9,
      className: 'packet-beam-line'
    }).addTo(layerGroupPacketAnim);

    if (tooltipText) {
      line.bindTooltip(tooltipText, {
        permanent: true,
        direction: 'center',
        className: 'packet-beam-tooltip'
      });
    }

    const photonIcon = L.divIcon({
      className: 'packet-photon-container',
      html: `<div class="packet-photon-dot" style="box-shadow:0 0 10px 4px ${color}, 0 0 20px 8px ${color};"></div>`,
      iconSize: [10, 10],
      iconAnchor: [5, 5]
    });

    const photonMarker = L.marker(latLng1, { icon: photonIcon }).addTo(layerGroupPacketAnim);

    let start = performance.now();
    const speed = 1200;

    function movePhoton(now) {
      const elapsed = now - start;
      const t = Math.min(elapsed / speed, 1);
      const curLat = latLng1[0] + t * (latLng2[0] - latLng1[0]);
      const curLng = latLng1[1] + t * (latLng2[1] - latLng1[1]);
      photonMarker.setLatLng([curLat, curLng]);

      if (t < 1) {
        requestAnimationFrame(movePhoton);
      } else {
        layerGroupPacketAnim.removeLayer(photonMarker);
      }
    }
    requestAnimationFrame(movePhoton);

    setTimeout(() => {
      let fadeStart = performance.now();
      function fade(now) {
        const p = Math.min((now - fadeStart) / 600, 1);
        line.setStyle({ opacity: 0.9 * (1 - p) });
        if (p < 1) {
          requestAnimationFrame(fade);
        } else {
          layerGroupPacketAnim.removeLayer(line);
        }
      }
      requestAnimationFrame(fade);
    }, 3800);
  }

  // ==========================================================================
  // SSE Real-time Updates Pipeline
  // ==========================================================================

  function initSSE() {
    if (state.eventSource) {
      try { state.eventSource.close(); } catch (_) {}
      state.eventSource = null;
    }

    try {
      const sse = new EventSource('/api/events');
      state.eventSource = sse;

      sse.onopen = () => {
        if (el.liveIndicator) {
          el.liveIndicator.querySelector('.live-dot').className = 'live-dot active';
          el.liveStatusText.textContent = 'LIVE';
        }
      };

      sse.onerror = () => {
        if (el.liveIndicator) {
          el.liveIndicator.querySelector('.live-dot').className = 'live-dot pulse';
          el.liveStatusText.textContent = 'RECONNECTING';
        }
      };

      // 1. Live Packet Activity
      sse.addEventListener('packet_activity', (e) => {
        try {
          const packet = JSON.parse(e.data);
          addPacketToHud(packet, true);
          animatePacketTransmission(packet);
        } catch (err) {
          console.error('Error handling packet_activity SSE:', err);
        }
      });

      // 2. Node updated
      sse.addEventListener('node_updated', (e) => {
        try {
          const data = JSON.parse(e.data);
          if (data.node) {
            state.nodes.set(data.node.node_num, data.node);
            renderMapNodes();
            renderNodesGrid();
            updateTelemetrySummaryCards();
          }
        } catch (err) {}
      });

      // 3. Position received
      sse.addEventListener('position_received', (e) => {
        try {
          const data = JSON.parse(e.data);
          const node = state.nodes.get(data.from_node);
          if (node) {
            node.latitude = data.latitude;
            node.longitude = data.longitude;
            node.altitude = data.altitude;
            renderMapNodes();
            fetchLocationHistory();
          }
        } catch (err) {}
      });

      // 4. Message received
      sse.addEventListener('message_received', (e) => {
        try {
          const m = JSON.parse(e.data);
          const activeTab = document.querySelector('.nav-tab.active')?.dataset.tab;
          if (activeTab !== 'messages') {
            state.unreadCount++;
            if (el.unreadCountBadge) {
              el.unreadCountBadge.style.display = 'inline-block';
              el.unreadCountBadge.textContent = state.unreadCount;
            }
          }
          loadMessages();
        } catch (err) {}
      });

      // 5. Traceroute received
      sse.addEventListener('traceroute_received', (e) => {
        try {
          const tr = JSON.parse(e.data);
          renderTracerouteResult(tr);
        } catch (err) {}
      });

      // 6. Raw packet
      sse.addEventListener('raw_packet', (e) => {
        try {
          const raw = JSON.parse(e.data);
          appendRawPacket(raw);
        } catch (err) {}
      });

    } catch (err) {
      console.warn('SSE initialization failed:', err);
    }
  }


  // ==========================================================================
  // Public Global API for Inline Callbacks
  // ==========================================================================
  window.meshApp = {
    openDM: function(nodeNum, nick) {
      state.selectedTarget = { kind: 'dm', target: nodeNum, title: `@${nick}` };
      el.chatTitle.textContent = `@${nick}`;
      el.chatSubtitle.textContent = `Direct Message with node ${nodeNum}`;
      document.querySelector('.nav-tab[data-tab="messages"]').click();
      loadMessages();
    },

    switchTarget: function(kind, target, title) {
      state.selectedTarget = { kind, target, title };
      el.chatTitle.textContent = title;
      el.chatSubtitle.textContent = kind === 'channel' ? 'Broadcast channel' : 'Direct Message';
      renderChannelList();
      loadMessages();
    },

    focusOnMap: function(nodeNum) {
      const node = state.nodes.get(nodeNum);
      if (!node || !node.latitude || !node.longitude) {
        alert('This node has not reported GPS coordinates yet.');
        return;
      }
      document.querySelector('.nav-tab[data-tab="map"]').click();
      setTimeout(() => {
        map.setView([node.latitude, node.longitude], 15);
        selectNodeForMap(node);
        const marker = state.nodeMarkers.get(nodeNum);
        if (marker) marker.openPopup();
      }, 300);
    },

    runTraceToNode: function(nodeNum) {
      document.querySelector('.nav-tab[data-tab="diagnostics"]').click();
      if (el.tracerouteTargetSelect) {
        el.tracerouteTargetSelect.value = nodeNum;
      }
      runTraceroute();
    },

    inspectTelemetry: function(nodeNum) {
      state.selectedNodeNumForTelemetry = nodeNum;
      document.querySelector('.nav-tab[data-tab="telemetry"]').click();
      if (el.telemetryNodeSelect) el.telemetryNodeSelect.value = nodeNum;
      updateTelemetrySummaryCards();
    },

    selectDevice: function(deviceId) {
      state.activeDeviceId = deviceId;
      el.deviceSelect.value = deviceId;
      loadConfig();
    },

    saveConfigKey: saveConfigKey
  };

  // --- Helpers ---
  function escapeHtml(str) {
    if (!str) return '';
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#039;');
  }

  function formatTimeAgo(ts) {
    if (!ts) return 'Never';
    const seconds = Math.floor(Date.now() / 1000) - ts;
    if (seconds < 60) return `${seconds}s ago`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `${minutes}m ago`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return `${hours}h ago`;
    const days = Math.floor(hours / 24);
    return `${days}d ago`;
  }

  // Run on DOM ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
