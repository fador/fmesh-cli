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
    telemetrySearchQuery: '',
    telemetryFilter: 'all',
    telemetrySort: 'last_heard',
    filteredTelemetryNodes: [],
    historyHours: 24,
    showLinks: true,
    showPacketAnim: true,
    showTrails: true,
    showLabels: true,
    darkMap: false,
    charts: {},
    myNodeNum: 0,
    myNodeId: '',
    nodeMarkers: new Map(),     // node_num -> Leaflet Marker
    nodeTrails: new Map(),      // node_num -> Leaflet Polyline
    rfLinks: [],                // array of raw link objects from /api/links
    rfLinkPolylines: [],        // array of Leaflet Polylines
    hasInitialMapFit: false,    // true once the map has initially fit bounds to nodes
    rawPackets: [],
    packetCount: 0,
    telemetryHistory: new Map(), // node_num -> Array<{ts, battery_level, voltage, temperature, relative_humidity, barometric_pressure, channel_util, air_util_tx}>
    telemetryLoadedNodes: new Set(), // Set<node_num> that have been fetched from /api/telemetry
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
    mapFloatingPanel: document.getElementById('map-floating-panel'),
    btnToggleMapOptions: document.getElementById('btn-toggle-map-options'),
    btnCloseMapOptions: document.getElementById('btn-close-map-options'),
    btnTogglePacketHud: document.getElementById('btn-toggle-packet-hud'),
    btnClosePacketHud: document.getElementById('btn-close-packet-hud'),
    mapBackdrop: document.getElementById('map-backdrop'),
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
    chatContainer: document.querySelector('.chat-container'),
    btnMobileChatBack: document.getElementById('btn-mobile-chat-back'),
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
    telemetryNodeSearch: document.getElementById('telemetry-node-search'),
    btnTelemetrySearchClear: document.getElementById('btn-telemetry-search-clear'),
    telemetryNodeSort: document.getElementById('telemetry-node-sort'),
    telemetryFilterChips: document.getElementById('telemetry-filter-chips'),
    btnPrevTelemetryNode: document.getElementById('btn-prev-telemetry-node'),
    btnNextTelemetryNode: document.getElementById('btn-next-telemetry-node'),
    telemetryNodesCountHint: document.getElementById('telemetry-nodes-count-hint'),
    metricsSummaryCards: document.getElementById('metrics-summary-cards'),
    overlayBattery: document.getElementById('overlay-battery'),
    overlayEnvironment: document.getElementById('overlay-environment'),
    overlayPressure: document.getElementById('overlay-pressure'),
    overlayUtilization: document.getElementById('overlay-utilization'),


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
    await fetchRfLinks();
    await fetchChannels();
    await fetchConversations();
    await fetchLocationHistory();
    await loadMessages();
    await fetchRecentPackets();
    await fetchRawPackets();
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
      if (el.mapFloatingPanel) el.mapFloatingPanel.classList.remove('open');
      if (el.packetActivityHud) el.packetActivityHud.classList.remove('open');
      if (el.mapBackdrop) el.mapBackdrop.classList.remove('visible');
      if (targetTab === 'messages') {
        state.unreadCount = 0;
        if (el.unreadCountBadge) el.unreadCountBadge.style.display = 'none';
        fetchChannels();
        fetchConversations();
        loadMessages();
        scrollToChatBottom();
      }
      if (targetTab === 'telemetry') {
        renderTelemetryNodePicker();
        updateTelemetryView();
      }
      if (targetTab === 'diagnostics') {
        fetchRawPackets();
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

    // If the user manually drags or zooms the map, mark that initial auto-fit is done
    map.on('movestart zoomstart', (e) => {
      if (e && e.originalEvent) {
        state.hasInitialMapFit = true;
      }
    });

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
    const isMyNode = Boolean(state.myNodeNum && node.node_num === state.myNodeNum);
    
    // Status color
    const now = Math.floor(Date.now() / 1000);
    const lastHeard = node.last_heard || 0;
    const diffMin = (now - lastHeard) / 60;
    
    let color = '#10b981'; // green (<15 min)
    if (isMyNode) color = '#3b82f6'; // vibrant blue for connected device
    else if (diffMin > 120) color = '#64748b'; // grey (>2 hours)
    else if (diffMin > 15) color = '#f59e0b'; // amber

    const pinGlow = isMyNode ? 'box-shadow: 0 0 14px rgba(59, 130, 246, 0.9), 0 0 4px #fff;' : '';
    const myBadge = isMyNode ? '<span class="my-node-badge">ME</span>' : '';

    const html = `
      <div class="custom-node-marker ${isMyNode ? 'my-node-marker' : ''}">
        <div class="marker-pin" style="background: ${color}; ${pinGlow}"></div>
        <span class="marker-avatar">${escapeHtml(shortName)}</span>
        ${myBadge}
        ${state.showLabels ? `<span class="marker-label ${isMyNode ? 'local-label' : ''}">${isMyNode ? '★ ' : ''}${escapeHtml(node.long_name || shortName)} (${battery}%)</span>` : ''}
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

    const bounds = [];
    const currentNodesWithPos = new Set();

    state.nodes.forEach(node => {
      if (node.latitude != null && node.longitude != null && node.latitude !== 0 && node.longitude !== 0) {
        const latLng = [node.latitude, node.longitude];
        bounds.push(latLng);
        currentNodesWithPos.add(node.node_num);

        const icon = createNodeMarkerIcon(node);
        const batteryText = node.battery_level != null ? `${node.battery_level}%` : 'Unknown';
        const snrText = node.snr != null ? `${node.snr.toFixed(1)} dB` : 'N/A';

        const popupHtml = `
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
        `;

        if (state.nodeMarkers.has(node.node_num)) {
          const existingMarker = state.nodeMarkers.get(node.node_num);
          const curPos = existingMarker.getLatLng();
          if (curPos.lat !== latLng[0] || curPos.lng !== latLng[1]) {
            existingMarker.setLatLng(latLng);
          }
          existingMarker.setIcon(icon);
          const popup = existingMarker.getPopup();
          if (popup) {
            popup.setContent(popupHtml);
          } else {
            existingMarker.bindPopup(popupHtml);
          }
        } else {
          const marker = L.marker(latLng, { icon: icon }).addTo(layerGroupNodes);
          marker.on('click', () => {
            selectNodeForMap(node);
          });
          marker.bindPopup(popupHtml);
          state.nodeMarkers.set(node.node_num, marker);
        }
      }
    });

    // Remove markers for nodes that no longer have positions or were removed
    for (const [nodeNum, marker] of state.nodeMarkers.entries()) {
      if (!currentNodesWithPos.has(nodeNum)) {
        layerGroupNodes.removeLayer(marker);
        state.nodeMarkers.delete(nodeNum);
      }
    }

    // Auto-fit bounds only on the initial data load, never on subsequent updates
    if (!state.hasInitialMapFit && bounds.length > 0) {
      try {
        map.fitBounds(bounds, { padding: [40, 40], maxZoom: 14 });
        state.hasInitialMapFit = true;
      } catch (_) {}
    }

    renderRfLinks();
  }

  async function fetchRfLinks() {
    try {
      const res = await fetch(`/api/links?device=${encodeURIComponent(state.activeDeviceId)}`);
      if (!res.ok) return;
      state.rfLinks = await res.json();
      renderRfLinks();
    } catch (err) {
      console.error('Error fetching RF links:', err);
    }
  }

  function renderRfLinks() {
    if (!map || !layerGroupLinks) return;
    layerGroupLinks.clearLayers();
    state.rfLinkPolylines = [];

    if (!state.showLinks || !Array.isArray(state.rfLinks) || state.rfLinks.length === 0) return;

    state.rfLinks.forEach(link => {
      const fromNode = state.nodes.get(link.from_node);
      const toNode = state.nodes.get(link.to_node);

      if (!fromNode || !toNode) return;
      if (fromNode.latitude == null || fromNode.longitude == null) return;
      if (toNode.latitude == null || toNode.longitude == null) return;
      if (fromNode.latitude === 0 && fromNode.longitude === 0) return;
      if (toNode.latitude === 0 && toNode.longitude === 0) return;

      const snr = link.snr != null ? link.snr : 0;
      let color = '#10b981'; // green (> 5 dB)
      if (snr < -5) color = '#f43f5e'; // red (< -5 dB)
      else if (snr < 5) color = '#f59e0b'; // amber (-5 to 5 dB)

      const isDirect = link.source === 'direct';
      const isTraceroute = link.source === 'traceroute';

      const line = L.polyline([
        [fromNode.latitude, fromNode.longitude],
        [toNode.latitude, toNode.longitude]
      ], {
        color: color,
        weight: Math.max(2, Math.min(5, 2.5 + (snr / 8))),
        opacity: 0.8,
        dashArray: isTraceroute ? '6, 6' : undefined
      }).addTo(layerGroupLinks);

      const fromName = fromNode.short_name || fromNode.long_name || link.from_id;
      const toName = toNode.short_name || toNode.long_name || link.to_id;
      const sourceLabel = isDirect ? 'Direct RF Neighbor (0 hops)' : (isTraceroute ? 'Traceroute Relay Hop' : (link.source === 'neighbor_info' ? 'Reported NeighborInfo' : 'Observed Packet Link'));
      const timeStr = formatTimeAgo(link.last_heard);

      line.bindTooltip(`
        <div class="rf-link-tooltip" style="font-size:0.75rem; line-height:1.4;">
          <div style="font-weight:700; color:var(--text-primary); margin-bottom:2px;">
            ${escapeHtml(fromName)} &harr; ${escapeHtml(toName)}
          </div>
          <div style="color:${color}; font-weight:600;">
            SNR: ${snr > 0 ? '+' : ''}${snr.toFixed(1)} dB
          </div>
          <div style="color:#94a3b8; font-size:0.7rem;">
            ${sourceLabel} • ${timeStr}
          </div>
        </div>
      `);
      state.rfLinkPolylines.push(line);
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
    if (el.mapBackdrop && window.innerWidth <= 768) {
      el.mapBackdrop.classList.add('visible');
    }

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
      if (res.ok) {
        const fetched = await res.json();
        if (Array.isArray(fetched) && fetched.length > 0) {
          fetched.forEach(ch => {
            const idx = state.channels.findIndex(c => c.index === ch.index);
            if (idx >= 0) {
              state.channels[idx] = { ...state.channels[idx], ...ch };
            } else {
              state.channels.push(ch);
            }
          });
        }
      }
      if (!state.channels.some(c => c.index === 0)) {
        state.channels.unshift({ index: 0, name: 'Primary', role: 'PRIMARY' });
      }
      state.channels.sort((a, b) => a.index - b.index);
      renderChannelList();

      if (state.selectedTarget.kind === 'channel') {
        const curCh = state.channels.find(c => c.index === state.selectedTarget.target);
        if (curCh && curCh.name) {
          state.selectedTarget.title = `#${curCh.name}`;
          if (el.chatTitle) el.chatTitle.textContent = `#${curCh.name} (Channel ${curCh.index})`;
          if (el.chatSubtitle) el.chatSubtitle.textContent = `Broadcast channel [${curCh.role || 'PRIMARY'}]`;
        }
      }
    } catch (err) {
      console.error('Error fetching channels:', err);
    }
  }

  function renderChannelList() {
    if (!el.channelList) return;
    const activeChannels = state.channels.filter(ch => ch.role !== 'DISABLED');
    const list = activeChannels.length > 0 ? activeChannels : (state.channels.length > 0 ? state.channels : [{ index: 0, name: 'Primary', role: 'PRIMARY' }]);
    el.channelList.innerHTML = list.map(ch => {
      const isActive = state.selectedTarget.kind === 'channel' && state.selectedTarget.target === ch.index;
      return `
        <li class="chat-target-item ${isActive ? 'active' : ''}" onclick="window.meshApp.switchTarget('channel', ${ch.index}, '#${escapeHtml(ch.name || 'Channel ' + ch.index)}')">
          <span>#${escapeHtml(ch.name || 'Channel ' + ch.index)}</span>
          <span class="node-badge">${escapeHtml(ch.role || '')}</span>
        </li>
      `;
    }).join('');
  }

  async function fetchConversations() {
    try {
      const res = await fetch(`/api/conversations?device=${encodeURIComponent(state.activeDeviceId)}`);
      if (!res.ok) return;
      const data = await res.json();
      state.activeDms = data.dms || [];
      if (Array.isArray(data.channels) && data.channels.length > 0) {
        data.channels.forEach(convCh => {
          const existing = state.channels.find(c => c.index === convCh.index);
          if (existing) {
            if (!existing.name && convCh.name) existing.name = convCh.name;
          } else {
            state.channels.push({
              index: convCh.index,
              name: convCh.name || ('Channel ' + convCh.index),
              role: convCh.index === 0 ? 'PRIMARY' : 'SECONDARY'
            });
          }
        });
        state.channels.sort((a, b) => a.index - b.index);
        renderChannelList();
      }
      renderDmList();
    } catch (err) {
      console.error('Error fetching conversations:', err);
    }
  }

  function renderDmList() {
    if (!el.dmList) return;
    const dms = state.activeDms || [];
    if (dms.length === 0) {
      el.dmList.innerHTML = '<li class="chat-target-item disabled" style="opacity:0.5; font-size:0.8rem; cursor:default;">No direct messages</li>';
      return;
    }
    el.dmList.innerHTML = dms.map(dm => {
      const isActive = state.selectedTarget.kind === 'dm' && state.selectedTarget.target === dm.node_num;
      const nick = dm.nick || `!${dm.node_num.toString(16)}`;
      return `
        <li class="chat-target-item ${isActive ? 'active' : ''}" onclick="window.meshApp.switchTarget('dm', ${dm.node_num}, '@${escapeHtml(nick)}')">
          <span>@${escapeHtml(nick)}</span>
          <span class="node-badge" style="font-family:monospace; font-size:0.65rem;">!${(dm.node_num).toString(16)}</span>
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
    if (!messages || messages.length === 0) {
      const targetName = state.selectedTarget.title || 'this conversation';
      el.chatMessagesContainer.innerHTML = `
        <div class="chat-empty-state">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
            <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path>
          </svg>
          <div class="chat-empty-title">No messages in ${escapeHtml(targetName)}</div>
          <div class="chat-empty-hint">${state.selectedTarget.kind === 'channel' ? 'Broadcast packets transmitted or received over LoRa will appear here live.' : 'Direct messages sent to or received from this node will appear here.'}</div>
        </div>
      `;
      return;
    }

    el.chatMessagesContainer.innerHTML = messages.map(m => {
      const isOut = m.direction === 'out';
      const senderNode = state.nodes.get(m.from_node);
      let senderName = '';
      if (isOut) {
        senderName = 'You';
      } else if (senderNode && (senderNode.short_name || senderNode.long_name)) {
        senderName = senderNode.short_name || senderNode.long_name;
      } else if (m.from_node) {
        senderName = '!' + m.from_node.toString(16).padStart(8, '0');
      } else {
        senderName = 'Unknown';
      }

      const timeStr = m.ts ? new Date(m.ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : '';

      let ackBadge = '';
      if (isOut && m.ack_state) {
        const color = m.ack_state === 'acked' ? 'var(--accent-emerald)' : m.ack_state === 'pending' ? 'var(--accent-amber)' : 'var(--accent-rose)';
        ackBadge = `<span style="font-size:0.65rem; color:${color}; margin-left:4px;">[${m.ack_state}]</span>`;
      }

      let sigBadge = '';
      if (!isOut) {
        const hopStart = m.hop_start || 0;
        const hopLimit = m.hop_limit || 0;
        const hops = m.hops !== undefined ? m.hops : ((hopStart > hopLimit) ? (hopStart - hopLimit) : 0);
        const snr = m.rx_snr !== undefined ? m.rx_snr : 0;
        const rssi = m.rx_rssi !== undefined ? m.rx_rssi : 0;
        const relay = m.relay_node || 0;

        if (hops === 0) {
          if (snr !== 0 || rssi !== 0 || hopStart > 0) {
            let parts = ['0-hop'];
            if (snr !== 0) parts.push(`${Number(snr).toFixed(1)}dB`);
            if (rssi !== 0) parts.push(`${rssi}dBm`);
            sigBadge = `<span class="msg-sig-badge sig-direct" title="Direct reception (0 hops)">${escapeHtml(parts.join(' '))}</span>`;
          }
        } else {
          let parts = [`${hops} ${hops === 1 ? 'hop' : 'hops'}`];
          if (relay) {
            let relayName = '';
            const rNode = state.nodes.get(relay);
            if (rNode) {
              relayName = rNode.short_name || rNode.long_name || ('!' + rNode.node_num.toString(16));
            } else if (relay <= 0xFF) {
              for (const [nNum, nObj] of state.nodes.entries()) {
                if ((nNum & 0xFF) === relay) {
                  relayName = nObj.short_name || nObj.long_name || ('!' + nNum.toString(16));
                  break;
                }
              }
              if (!relayName) relayName = `!*${relay.toString(16).padStart(2, '0')}`;
            } else {
              relayName = '!' + relay.toString(16).padStart(8, '0');
            }
            parts.push(`via ${relayName}`);
          }
          if (snr !== 0) parts.push(`${Number(snr).toFixed(1)}dB`);
          if (rssi !== 0) parts.push(`${rssi}dBm`);
          sigBadge = `<span class="msg-sig-badge sig-relayed" title="Multi-hop relayed message">${escapeHtml(parts.join(' '))}</span>`;
        }
      }

      return `
        <div class="chat-message-row ${isOut ? 'outgoing' : 'incoming'}">
          <div class="chat-message-meta">
            <span class="chat-sender-nick">${escapeHtml(senderName)}</span>
            <span>${timeStr}</span>
            ${ackBadge}
            ${sigBadge}
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
  // Telemetry Dashboard & Charts (Real Data Only)
  // ==========================================================================
  async function loadTelemetryForNode(nodeNum) {
    if (!nodeNum || state.telemetryLoadedNodes.has(nodeNum)) return;
    state.telemetryLoadedNodes.add(nodeNum);
    try {
      const res = await fetch(`/api/telemetry?node_num=${nodeNum}&limit=500`);
      if (!res.ok) return;
      const data = await res.json();
      if (!Array.isArray(data)) return;

      const dbSamples = data.map(item => ({
        ts: item.ts,
        battery_level: item.battery_level,
        voltage: item.voltage,
        temperature: item.temperature,
        relative_humidity: item.relative_humidity,
        barometric_pressure: item.barometric_pressure,
        channel_util: item.channel_util,
        air_util_tx: item.air_util_tx
      }));

      const existing = state.telemetryHistory.get(nodeNum) || [];
      const sampleMap = new Map();
      dbSamples.forEach(s => sampleMap.set(s.ts, s));
      existing.forEach(s => {
        if (!sampleMap.has(s.ts)) {
          sampleMap.set(s.ts, s);
        } else {
          const curr = sampleMap.get(s.ts);
          sampleMap.set(s.ts, Object.assign({}, curr, s));
        }
      });

      const merged = Array.from(sampleMap.values()).sort((a, b) => a.ts - b.ts);
      if (merged.length > 500) merged.splice(0, merged.length - 500);
      state.telemetryHistory.set(nodeNum, merged);

      if (state.selectedNodeNumForTelemetry === nodeNum) {
        updateTelemetryCharts(state.nodes.get(nodeNum));
      }
    } catch (e) {
      console.warn('Failed to load historical telemetry for node:', nodeNum, e);
    }
  }

  function recordTelemetrySample(node) {
    if (!node) return;
    const hasAny = node.battery_level != null || node.voltage != null ||
                   node.temperature != null || node.relative_humidity != null ||
                   node.barometric_pressure != null || node.channel_util != null ||
                   node.air_util_tx != null;
    if (!hasAny) return;

    if (!state.telemetryHistory.has(node.node_num)) {
      state.telemetryHistory.set(node.node_num, []);
    }
    const samples = state.telemetryHistory.get(node.node_num);
    const ts = node.last_heard || Math.floor(Date.now() / 1000);

    if (samples.length > 0 && samples[samples.length - 1].ts === ts) {
      samples[samples.length - 1] = {
        ts,
        battery_level: node.battery_level,
        voltage: node.voltage,
        temperature: node.temperature,
        relative_humidity: node.relative_humidity,
        barometric_pressure: node.barometric_pressure,
        channel_util: node.channel_util,
        air_util_tx: node.air_util_tx
      };
    } else {
      samples.push({
        ts,
        battery_level: node.battery_level,
        voltage: node.voltage,
        temperature: node.temperature,
        relative_humidity: node.relative_humidity,
        barometric_pressure: node.barometric_pressure,
        channel_util: node.channel_util,
        air_util_tx: node.air_util_tx
      });
      if (samples.length > 500) samples.shift();
    }
  }

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

    // 1. Battery & Voltage Chart (Initialized empty - real data only)
    const ctxBattery = document.getElementById('chart-battery')?.getContext('2d');
    if (ctxBattery) {
      state.charts.battery = new Chart(ctxBattery, {
        type: 'line',
        data: {
          labels: [],
          datasets: [
            { label: 'Battery (%)', data: [], borderColor: '#10b981', backgroundColor: 'rgba(16,185,129,0.1)', tension: 0.3 },
            { label: 'Voltage (V)', data: [], borderColor: '#06b6d4', yAxisID: 'y1', tension: 0.3 }
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

    // 2. Environment Chart (Temp & Humidity - Initialized empty)
    const ctxEnv = document.getElementById('chart-environment')?.getContext('2d');
    if (ctxEnv) {
      state.charts.environment = new Chart(ctxEnv, {
        type: 'line',
        data: {
          labels: [],
          datasets: [
            { label: 'Temperature (°C)', data: [], borderColor: '#f59e0b', backgroundColor: 'rgba(245,158,11,0.1)', tension: 0.3 },
            { label: 'Humidity (%)', data: [], borderColor: '#3b82f6', tension: 0.3 }
          ]
        },
        options: chartOptions
      });
    }

    // 3. Pressure Chart (Initialized empty)
    const ctxPressure = document.getElementById('chart-pressure')?.getContext('2d');
    if (ctxPressure) {
      state.charts.pressure = new Chart(ctxPressure, {
        type: 'line',
        data: {
          labels: [],
          datasets: [
            { label: 'Pressure (hPa)', data: [], borderColor: '#8b5cf6', tension: 0.3 }
          ]
        },
        options: chartOptions
      });
    }

    // 4. Channel Utilization (Initialized empty)
    const ctxUtil = document.getElementById('chart-utilization')?.getContext('2d');
    if (ctxUtil) {
      state.charts.utilization = new Chart(ctxUtil, {
        type: 'bar',
        data: {
          labels: [],
          datasets: [
            { label: 'Channel Util (%)', data: [], backgroundColor: '#06b6d4' },
            { label: 'Air Time TX (%)', data: [], backgroundColor: '#10b981' }
          ]
        },
        options: chartOptions
      });
    }

    renderTelemetryNodePicker();
    updateTelemetryView();
  }

  function updateTelemetryView(overrideNode) {
    let targetNode = overrideNode !== undefined ? overrideNode : null;
    if (targetNode === null && state.selectedNodeNumForTelemetry) {
      targetNode = state.nodes.get(state.selectedNodeNumForTelemetry);
    }
    if (!targetNode && state.nodes.size > 0 && overrideNode === undefined) {
      const first = (state.filteredTelemetryNodes && state.filteredTelemetryNodes.length > 0)
        ? state.filteredTelemetryNodes[0]
        : Array.from(state.nodes.values())[0];
      targetNode = first;
      state.selectedNodeNumForTelemetry = targetNode ? targetNode.node_num : null;
    }

    if (el.telemetryNodeSelect && targetNode) {
      el.telemetryNodeSelect.value = targetNode.node_num;
    }

    if (targetNode && !state.telemetryLoadedNodes.has(targetNode.node_num)) {
      loadTelemetryForNode(targetNode.node_num);
    }

    updateTelemetrySummaryCards(targetNode);
    updateTelemetryCharts(targetNode);
  }

  function renderTelemetryNodePicker() {
    const query = (state.telemetrySearchQuery || '').toLowerCase().trim();
    const filter = state.telemetryFilter || 'all';
    const sortKey = state.telemetrySort || 'last_heard';
    const now = Math.floor(Date.now() / 1000);

    let list = Array.from(state.nodes.values());

    // 1. Text Search Filter (name, short name, node_id, hardware model, role)
    if (query) {
      list = list.filter(n =>
        (n.long_name && n.long_name.toLowerCase().includes(query)) ||
        (n.short_name && n.short_name.toLowerCase().includes(query)) ||
        (n.node_id && n.node_id.toLowerCase().includes(query)) ||
        (n.hw_model && n.hw_model.toLowerCase().includes(query)) ||
        (n.role && n.role.toLowerCase().includes(query))
      );
    }

    // 2. Organization / Category Filter
    if (filter === 'has_telemetry') {
      list = list.filter(n => {
        const hasProps = n.battery_level != null || n.voltage != null || n.temperature != null ||
                         n.relative_humidity != null || n.barometric_pressure != null ||
                         n.channel_util != null || n.air_util_tx != null;
        const hasHistory = state.telemetryHistory.has(n.node_num) && state.telemetryHistory.get(n.node_num).length > 0;
        return hasProps || hasHistory;
      });
    } else if (filter === 'recent') {
      list = list.filter(n => (now - (n.last_heard || 0)) < 3600);
    } else if (filter === 'favorites') {
      list = list.filter(n => n.is_favorite);
    } else if (filter === 'routers') {
      list = list.filter(n => n.role && (n.role.includes('ROUTER') || n.role.includes('REPEATER')));
    }

    // 3. Sort Order
    list.sort((a, b) => {
      if (sortKey === 'name') {
        const nameA = a.long_name || a.short_name || '';
        const nameB = b.long_name || b.short_name || '';
        return nameA.localeCompare(nameB);
      }
      if (sortKey === 'battery') {
        return (b.battery_level != null ? b.battery_level : -1) - (a.battery_level != null ? a.battery_level : -1);
      }
      if (sortKey === 'hops') {
        return (a.hops_away != null ? a.hops_away : 99) - (b.hops_away != null ? b.hops_away : 99);
      }
      if (sortKey === 'snr') {
        return (b.snr != null ? b.snr : -99) - (a.snr != null ? a.snr : -99);
      }
      return (b.last_heard || 0) - (a.last_heard || 0);
    });

    state.filteredTelemetryNodes = list;

    // Update count hint
    if (el.telemetryNodesCountHint) {
      const total = state.nodes.size;
      el.telemetryNodesCountHint.textContent = `${list.length} of ${total} nodes`;
    }

    // Clear button toggle
    if (el.btnTelemetrySearchClear) {
      el.btnTelemetrySearchClear.style.display = query ? 'block' : 'none';
    }

    if (!el.telemetryNodeSelect) return;

    if (list.length === 0) {
      el.telemetryNodeSelect.innerHTML = '<option value="">No matching nodes found</option>';
      state.selectedNodeNumForTelemetry = null;
      updateTelemetryView(null);
      return;
    }

    function buildOption(n) {
      const isMe = state.myNodeNum && n.node_num === state.myNodeNum;
      const name = n.long_name || n.short_name || n.node_id;
      const parts = [];
      if (isMe) parts.push('★ ME');
      parts.push(name);
      parts.push(`(${n.node_id})`);
      if (n.battery_level != null) parts.push(`🔋${n.battery_level}%`);
      if (n.temperature != null) parts.push(`🌡️${n.temperature.toFixed(1)}°C`);
      if (n.hops_away != null) parts.push(n.hops_away === 0 ? '0-hop' : `${n.hops_away}h`);
      return `<option value="${n.node_num}">${escapeHtml(parts.join(' '))}</option>`;
    }

    const myNode = list.find(n => state.myNodeNum && n.node_num === state.myNodeNum);
    const telemetryNodes = list.filter(n => n !== myNode && (
      n.battery_level != null || n.temperature != null || n.voltage != null ||
      (state.telemetryHistory.has(n.node_num) && state.telemetryHistory.get(n.node_num).length > 0)
    ));
    const otherNodes = list.filter(n => n !== myNode && !telemetryNodes.includes(n));

    let html = '';
    if (myNode) {
      html += `<optgroup label="Connected Device">${buildOption(myNode)}</optgroup>`;
    }
    if (telemetryNodes.length > 0) {
      html += `<optgroup label="Nodes With Telemetry (${telemetryNodes.length})">${telemetryNodes.map(buildOption).join('')}</optgroup>`;
    }
    if (otherNodes.length > 0) {
      html += `<optgroup label="Other Mesh Nodes (${otherNodes.length})">${otherNodes.map(buildOption).join('')}</optgroup>`;
    }

    el.telemetryNodeSelect.innerHTML = html;

    const hasCurrent = list.some(n => n.node_num === state.selectedNodeNumForTelemetry);
    if (!hasCurrent) {
      state.selectedNodeNumForTelemetry = list[0].node_num;
    }
    el.telemetryNodeSelect.value = state.selectedNodeNumForTelemetry;
    updateTelemetryView();
  }

  function stepTelemetryNode(direction) {
    const list = state.filteredTelemetryNodes;
    if (!list || list.length === 0) return;
    const curIdx = list.findIndex(n => n.node_num === state.selectedNodeNumForTelemetry);
    let nextIdx = 0;
    if (curIdx >= 0) {
      nextIdx = (curIdx + direction + list.length) % list.length;
    }
    state.selectedNodeNumForTelemetry = list[nextIdx].node_num;
    if (el.telemetryNodeSelect) {
      el.telemetryNodeSelect.value = state.selectedNodeNumForTelemetry;
    }
    updateTelemetryView();
  }

  function updateTelemetrySummaryCards(targetNode) {
    if (!el.metricsSummaryCards) return;

    if (!targetNode) {
      el.metricsSummaryCards.innerHTML = '<div class="empty-state">No node selected for telemetry.</div>';
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

  function updateTelemetryCharts(targetNode) {
    if (!state.charts.battery) return;

    if (!targetNode) {
      ['battery', 'environment', 'pressure', 'utilization'].forEach(key => {
        if (state.charts[key]) {
          state.charts[key].data.labels = [];
          state.charts[key].data.datasets.forEach(ds => ds.data = []);
          state.charts[key].update();
        }
      });
      if (el.overlayBattery) el.overlayBattery.style.display = 'flex';
      if (el.overlayEnvironment) el.overlayEnvironment.style.display = 'flex';
      if (el.overlayPressure) el.overlayPressure.style.display = 'flex';
      if (el.overlayUtilization) el.overlayUtilization.style.display = 'flex';
      return;
    }

    recordTelemetrySample(targetNode);
    const samples = state.telemetryHistory.get(targetNode.node_num) || [];

    const formatTimeLabel = (ts) => {
      if (!ts) return '';
      const d = new Date(ts * 1000);
      const now = new Date();
      if (d.toDateString() !== now.toDateString()) {
        return `${d.getMonth() + 1}/${d.getDate()} ${d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
      }
      return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    };

    // 1. Battery & Voltage Chart (Real Data Only)
    const batterySamples = samples.filter(s => s.battery_level != null || s.voltage != null);
    if (batterySamples.length > 0) {
      if (el.overlayBattery) el.overlayBattery.style.display = 'none';
      state.charts.battery.data.labels = batterySamples.map(s => formatTimeLabel(s.ts));
      state.charts.battery.data.datasets[0].data = batterySamples.map(s => s.battery_level != null ? s.battery_level : null);
      state.charts.battery.data.datasets[1].data = batterySamples.map(s => s.voltage != null ? s.voltage : null);
    } else {
      if (el.overlayBattery) el.overlayBattery.style.display = 'flex';
      state.charts.battery.data.labels = [];
      state.charts.battery.data.datasets[0].data = [];
      state.charts.battery.data.datasets[1].data = [];
    }
    state.charts.battery.update();

    // 2. Environment Chart (Temp & Humidity - Real Data Only)
    const envSamples = samples.filter(s => s.temperature != null || s.relative_humidity != null);
    if (envSamples.length > 0) {
      if (el.overlayEnvironment) el.overlayEnvironment.style.display = 'none';
      state.charts.environment.data.labels = envSamples.map(s => formatTimeLabel(s.ts));
      state.charts.environment.data.datasets[0].data = envSamples.map(s => s.temperature != null ? s.temperature : null);
      state.charts.environment.data.datasets[1].data = envSamples.map(s => s.relative_humidity != null ? s.relative_humidity : null);
    } else {
      if (el.overlayEnvironment) el.overlayEnvironment.style.display = 'flex';
      state.charts.environment.data.labels = [];
      state.charts.environment.data.datasets[0].data = [];
      state.charts.environment.data.datasets[1].data = [];
    }
    state.charts.environment.update();

    // 3. Pressure Chart (Real Data Only)
    const pressSamples = samples.filter(s => s.barometric_pressure != null);
    if (pressSamples.length > 0) {
      if (el.overlayPressure) el.overlayPressure.style.display = 'none';
      state.charts.pressure.data.labels = pressSamples.map(s => formatTimeLabel(s.ts));
      state.charts.pressure.data.datasets[0].data = pressSamples.map(s => s.barometric_pressure);
    } else {
      if (el.overlayPressure) el.overlayPressure.style.display = 'flex';
      state.charts.pressure.data.labels = [];
      state.charts.pressure.data.datasets[0].data = [];
    }
    state.charts.pressure.update();

    // 4. Channel Utilization (Real Data Only)
    const hasUtil = targetNode.channel_util != null || targetNode.air_util_tx != null;
    if (hasUtil) {
      if (el.overlayUtilization) el.overlayUtilization.style.display = 'none';
      state.charts.utilization.data.labels = [targetNode.short_name || targetNode.node_id];
      state.charts.utilization.data.datasets[0].data = [targetNode.channel_util != null ? targetNode.channel_util : 0];
      state.charts.utilization.data.datasets[1].data = [targetNode.air_util_tx != null ? targetNode.air_util_tx : 0];
    } else {
      if (el.overlayUtilization) el.overlayUtilization.style.display = 'flex';
      state.charts.utilization.data.labels = [];
      state.charts.utilization.data.datasets[0].data = [];
      state.charts.utilization.data.datasets[1].data = [];
    }
    state.charts.utilization.update();
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

  function renderTracerouteResult(ev) {
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
        <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:1rem;">Target: ${escapeHtml(ev.from_id || ev.to_id || '')}</div>
        ${hopsHtml || '<div class="empty-state">Direct 1-hop link (no intermediate repeaters).</div>'}
      </div>
    `;
  }
  const handleTracerouteReceived = renderTracerouteResult;

  async function fetchRawPackets() {
    try {
      const res = await fetch(`/api/raw?device=${encodeURIComponent(state.activeDeviceId)}`);
      if (!res.ok) return;
      const list = await res.json();
      if (!el.rawPacketStream) return;
      el.rawPacketStream.innerHTML = '';
      state.rawPackets = [];
      list.forEach(pkt => handleRawPacket(pkt));
    } catch (err) {
      console.error('Error fetching raw packets:', err);
    }
  }

  function handleRawPacket(pkt) {
    if (!el.rawPacketStream) return;

    state.rawPackets.unshift(pkt);
    if (state.rawPackets.length > 50) state.rawPackets.pop();

    const timeStr = pkt.ts ? new Date(pkt.ts * 1000).toLocaleTimeString() : new Date().toLocaleTimeString();
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
  const appendRawPacket = handleRawPacket;

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

  function renderConfigTable(items) {
    if (!el.configTableBody) return;
    const search = el.configSearch.value.toLowerCase().trim();

    const normalized = (items || []).map(item => {
      if (typeof item === 'string') {
        const parts = item.split('=');
        return {
          key: parts[0]?.trim() || '',
          value: parts.slice(1).join('=').trim()
        };
      }
      return {
        key: item.key || '',
        value: item.value != null ? String(item.value) : ''
      };
    });

    el.configTableBody.innerHTML = normalized
      .filter(item => !search || item.key.toLowerCase().includes(search) || item.value.toLowerCase().includes(search))
      .map(({ key, value }) => {
        return `
          <tr>
            <td class="config-key">${escapeHtml(key)}</td>
            <td>
              <input type="text" class="config-val-input" id="cfg-val-${escapeHtml(key)}" value="${escapeHtml(value)}">
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
  // Fetch APIs
  // ==========================================================================
  async function fetchStatus() {
    try {
      const res = await fetch('/api/status');
      if (!res.ok) return;
      const data = await res.json();
      state.activeDeviceId = data.active_device || '';
      if (data.my_node_num) {
        state.myNodeNum = data.my_node_num;
        state.myNodeId = data.my_node_id || '';
      }
    } catch (err) {
      console.error('Error fetching status:', err);
    }
  }

  async function fetchDevices() {
    try {
      const res = await fetch('/api/devices');
      if (!res.ok) return;
      state.devices = await res.json();

      const cur = state.devices.find(d => d.id === state.activeDeviceId);
      if (cur && cur.my_node_num) {
        state.myNodeNum = cur.my_node_num;
        state.myNodeId = cur.my_node_id || '';
      }

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
      list.forEach(n => {
        state.nodes.set(n.node_num, n);
        recordTelemetrySample(n);
      });

      renderMapNodes();
      renderNodesGrid();
      updateNodePickers();
      updateTelemetryView();
    } catch (err) {
      console.error('Error fetching nodes:', err);
    }
  }

  function updateNodePickers() {
    renderTelemetryNodePicker();

    const options = Array.from(state.nodes.values()).map(n => `
      <option value="${n.node_num}">${escapeHtml(n.long_name || n.short_name)} (${n.node_id})</option>
    `).join('');

    if (el.tracerouteTargetSelect) el.tracerouteTargetSelect.innerHTML = options;
  }

  // ==========================================================================
  // Event Listeners
  // ==========================================================================
  function initEventListeners() {
    // Device select
    el.deviceSelect?.addEventListener('change', async () => {
      state.activeDeviceId = el.deviceSelect.value;
      state.hasInitialMapFit = false;
      await fetch('/api/devices/select', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device: state.activeDeviceId })
      });
      await fetchNodes();
      await fetchRfLinks();
      await fetchChannels();
      await loadMessages();
      await fetchRawPackets();
    });

    // Telemetry node select, search, sort, filters, stepper
    el.telemetryNodeSelect?.addEventListener('change', () => {
      const val = parseInt(el.telemetryNodeSelect.value, 10);
      if (!isNaN(val)) {
        state.selectedNodeNumForTelemetry = val;
        updateTelemetryView();
      }
    });

    el.telemetryNodeSearch?.addEventListener('input', () => {
      state.telemetrySearchQuery = el.telemetryNodeSearch.value;
      renderTelemetryNodePicker();
    });

    el.btnTelemetrySearchClear?.addEventListener('click', () => {
      state.telemetrySearchQuery = '';
      if (el.telemetryNodeSearch) el.telemetryNodeSearch.value = '';
      renderTelemetryNodePicker();
    });

    el.telemetryNodeSort?.addEventListener('change', () => {
      state.telemetrySort = el.telemetryNodeSort.value;
      renderTelemetryNodePicker();
    });

    document.querySelectorAll('#telemetry-filter-chips .filter-chip').forEach(chip => {
      chip.addEventListener('click', () => {
        document.querySelectorAll('#telemetry-filter-chips .filter-chip').forEach(c => c.classList.remove('active'));
        chip.classList.add('active');
        state.telemetryFilter = chip.dataset.tfilter || 'all';
        renderTelemetryNodePicker();
      });
    });

    el.btnPrevTelemetryNode?.addEventListener('click', () => {
      stepTelemetryNode(-1);
    });

    el.btnNextTelemetryNode?.addEventListener('click', () => {
      stepTelemetryNode(1);
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
      if (el.mapBackdrop) el.mapBackdrop.classList.remove('visible');
      state.selectedNodeNumForMap = null;
    });

    // Mobile Chat Back Button
    el.btnMobileChatBack?.addEventListener('click', () => {
      if (el.chatContainer) {
        el.chatContainer.classList.remove('chat-active');
      }
    });

    // Mobile Map Controls Toggles
    el.btnToggleMapOptions?.addEventListener('click', () => {
      const isOpen = el.mapFloatingPanel?.classList.toggle('open');
      if (isOpen) {
        el.packetActivityHud?.classList.remove('open');
        el.mapBackdrop?.classList.add('visible');
      } else {
        el.mapBackdrop?.classList.remove('visible');
      }
    });

    el.btnTogglePacketHud?.addEventListener('click', () => {
      const isOpen = el.packetActivityHud?.classList.toggle('open');
      if (isOpen) {
        el.mapFloatingPanel?.classList.remove('open');
        el.mapBackdrop?.classList.add('visible');
      } else {
        el.mapBackdrop?.classList.remove('visible');
      }
    });

    el.btnCloseMapOptions?.addEventListener('click', () => {
      el.mapFloatingPanel?.classList.remove('open');
      el.mapBackdrop?.classList.remove('visible');
    });

    el.btnClosePacketHud?.addEventListener('click', () => {
      el.packetActivityHud?.classList.remove('open');
      el.mapBackdrop?.classList.remove('visible');
    });

    el.mapBackdrop?.addEventListener('click', () => {
      el.mapFloatingPanel?.classList.remove('open');
      el.packetActivityHud?.classList.remove('open');
      el.mapNodeDrawer?.classList.add('collapsed');
      el.mapBackdrop?.classList.remove('visible');
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
            recordTelemetrySample(data.node);
            renderMapNodes();
            renderNodesGrid();
            if (state.selectedNodeNumForTelemetry === data.node.node_num) {
              updateTelemetryView();
            }
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
          fetchConversations();
          loadMessages();
        } catch (err) {}
      });

      // 5. ACK received
      sse.addEventListener('ack_received', () => {
        try {
          loadMessages();
        } catch (err) {}
      });

      // 6. Traceroute received
      sse.addEventListener('traceroute_received', (e) => {
        try {
          const tr = JSON.parse(e.data);
          renderTracerouteResult(tr);
          fetchRfLinks();
        } catch (err) {}
      });

      // 7. RF Link updated
      sse.addEventListener('link_updated', (e) => {
        try {
          const link = JSON.parse(e.data);
          const k1 = Math.min(link.from_node, link.to_node);
          const k2 = Math.max(link.from_node, link.to_node);
          const idx = state.rfLinks.findIndex(l => Math.min(l.from_node, l.to_node) === k1 && Math.max(l.from_node, l.to_node) === k2);
          if (idx >= 0) {
            state.rfLinks[idx] = link;
          } else {
            state.rfLinks.push(link);
          }
          renderRfLinks();
        } catch (err) {}
      });

      // 8. Raw packet
      sse.addEventListener('raw_packet', (e) => {
        try {
          const raw = JSON.parse(e.data);
          handleRawPacket(raw);
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
      const displayNick = nick || `!${nodeNum.toString(16)}`;
      state.selectedTarget = { kind: 'dm', target: nodeNum, title: `@${displayNick}` };
      el.chatTitle.textContent = `@${displayNick}`;
      el.chatSubtitle.textContent = `Direct Message with node ${nodeNum}`;
      renderChannelList();
      renderDmList();
      if (el.chatContainer) {
        el.chatContainer.classList.add('chat-active');
      }
      document.querySelector('.nav-tab[data-tab="messages"]').click();
      loadMessages();
    },

    switchTarget: function(kind, target, title) {
      state.selectedTarget = { kind, target, title };
      el.chatTitle.textContent = title + (kind === 'channel' ? ` (Channel ${target})` : '');
      el.chatSubtitle.textContent = kind === 'channel' ? 'Broadcast channel' : `Direct Message with node ${target}`;
      renderChannelList();
      renderDmList();
      if (el.chatContainer) {
        el.chatContainer.classList.add('chat-active');
      }
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
      state.telemetrySearchQuery = '';
      if (el.telemetryNodeSearch) el.telemetryNodeSearch.value = '';
      state.telemetryFilter = 'all';
      document.querySelectorAll('#telemetry-filter-chips .filter-chip').forEach(c => {
        c.classList.toggle('active', c.dataset.tfilter === 'all');
      });
      document.querySelector('.nav-tab[data-tab="telemetry"]').click();
      renderTelemetryNodePicker();
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
