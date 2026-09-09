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
    statsRange: '1d',
    statsNodeFilter: 0,
    statsSearchQuery: '',
    statsData: null,
    configData: null,
    configSections: {},
    configModified: new Map(),
    activeConfigSection: 'all',
    configSearchQuery: '',
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
    configDevicesCount: document.getElementById('config-devices-count'),
    configDeviceTitle: document.getElementById('config-device-title'),
    configDeviceBadges: document.getElementById('config-device-badges'),
    btnRefreshConfig: document.getElementById('btn-refresh-config'),
    btnSaveAllConfig: document.getElementById('btn-save-all-config'),
    configCategoryPills: document.getElementById('config-category-pills'),
    configSearch: document.getElementById('config-search'),
    btnConfigSearchClear: document.getElementById('btn-config-search-clear'),
    configCardsContainer: document.getElementById('config-cards-container'),
    configRawContainer: document.getElementById('config-raw-container'),
    configTableBody: document.getElementById('config-table-body'),

    // Statistics
    statsRangeChips: document.getElementById('stats-range-chips'),
    statsNodeSelect: document.getElementById('stats-node-select'),
    btnRefreshStats: document.getElementById('btn-refresh-stats'),
    statsTotalPackets: document.getElementById('stats-total-packets'),
    statsCastRatio: document.getElementById('stats-cast-ratio'),
    statsActiveNodes: document.getElementById('stats-active-nodes'),
    statsMsgCount: document.getElementById('stats-msg-count'),
    statsTelemetryCount: document.getElementById('stats-telemetry-count'),
    statsPosCount: document.getElementById('stats-pos-count'),
    statsAvgSnr: document.getElementById('stats-avg-snr'),
    statsTimelineResolution: document.getElementById('stats-timeline-resolution'),
    statsTableBody: document.getElementById('stats-table-body'),
    statsTableCount: document.getElementById('stats-table-count'),
    statsTableSearch: document.getElementById('stats-table-search'),
    btnStatsSearchClear: document.getElementById('btn-stats-search-clear'),
    overlayStatsTimeline: document.getElementById('overlay-stats-timeline'),
    overlayStatsDoughnut: document.getElementById('overlay-stats-doughnut'),
    overlayStatsNodes: document.getElementById('overlay-stats-nodes')
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
    initStatsCharts();
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
      if (targetTab === 'stats') {
        updateNodePickers();
        fetchAndRenderStats();
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

    // Re-render co-located marker positions on zoom so pixel separation remains consistent
    map.on('zoomend', () => {
      renderMapNodes();
    });
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

  // Displace co-located nodes slightly so all overlapping markers remain visible and clickable
  function calculateDisplacedPositions() {
    const nodesWithPos = [];
    state.nodes.forEach(node => {
      if (node.latitude != null && node.longitude != null && node.latitude !== 0 && node.longitude !== 0) {
        nodesWithPos.push(node);
      }
    });

    // Group nodes that share the same or virtually identical location (< ~15 meters)
    const clusters = [];
    nodesWithPos.forEach(node => {
      let cluster = null;
      for (const c of clusters) {
        const leader = c[0];
        // ~15m threshold (0.00015 deg lat/lng)
        if (Math.abs(leader.latitude - node.latitude) < 0.00015 &&
            Math.abs(leader.longitude - node.longitude) < 0.00015) {
          cluster = c;
          break;
        }
      }
      if (cluster) {
        cluster.push(node);
      } else {
        clusters.push([node]);
      }
    });

    const positions = new Map(); // node_num -> [lat, lng]

    clusters.forEach(cluster => {
      if (cluster.length === 1) {
        const n = cluster[0];
        positions.set(n.node_num, [n.latitude, n.longitude]);
        return;
      }

      // Stable sort: keep local node (ME) first if present, then by node_num
      cluster.sort((a, b) => {
        const aIsMe = state.myNodeNum && a.node_num === state.myNodeNum ? 1 : 0;
        const bIsMe = state.myNodeNum && b.node_num === state.myNodeNum ? 1 : 0;
        if (aIsMe !== bIsMe) return bIsMe - aIsMe;
        return a.node_num - b.node_num;
      });

      const baseLat = cluster[0].latitude;
      const baseLng = cluster[0].longitude;
      const N = cluster.length;

      // Marker pin is 32px wide; radius of 24px gives 48px center distance for 2 nodes
      const pixelRadius = Math.min(50, 20 + N * 3);

      const canUsePixel = Boolean(map && map._loaded && typeof map.latLngToLayerPoint === 'function');
      let basePoint = null;
      if (canUsePixel) {
        try {
          basePoint = map.latLngToLayerPoint([baseLat, baseLng]);
        } catch (_) {
          basePoint = null;
        }
      }

      cluster.forEach((node, i) => {
        let angle;
        if (N === 2) {
          // Horizontal side-by-side layout: left (-23px) and right (+23px)
          angle = i === 0 ? Math.PI : 0;
        } else {
          // Circular distribution around base point, starting from top
          angle = (2 * Math.PI * i) / N - Math.PI / 2;
        }

        if (basePoint) {
          const offsetX = pixelRadius * Math.cos(angle);
          const offsetY = pixelRadius * Math.sin(angle);
          const pt = L.point(basePoint.x + offsetX, basePoint.y + offsetY);
          const latLng = map.layerPointToLatLng(pt);
          positions.set(node.node_num, [latLng.lat, latLng.lng]);
        } else {
          // Fallback: geographic degree offset (~20m)
          const geoRadius = 0.0002;
          const dLat = geoRadius * Math.sin(angle);
          const dLng = (geoRadius * Math.cos(angle)) / Math.max(0.1, Math.cos((baseLat * Math.PI) / 180));
          positions.set(node.node_num, [baseLat + dLat, baseLng + dLng]);
        }
      });
    });

    return positions;
  }

  function renderMapNodes() {
    if (!map) return;

    const bounds = [];
    const currentNodesWithPos = new Set();
    const positions = calculateDisplacedPositions();

    state.nodes.forEach(node => {
      if (positions.has(node.node_num)) {
        const latLng = positions.get(node.node_num);
        bounds.push([node.latitude, node.longitude]);
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

      const fromMarker = state.nodeMarkers.get(link.from_node);
      const toMarker = state.nodeMarkers.get(link.to_node);
      const fromPos = fromMarker ? fromMarker.getLatLng() : [fromNode.latitude, fromNode.longitude];
      const toPos = toMarker ? toMarker.getLatLng() : [toNode.latitude, toNode.longitude];

      const snr = link.snr != null ? link.snr : 0;
      let color = '#10b981'; // green (> 5 dB)
      if (snr < -5) color = '#f43f5e'; // red (< -5 dB)
      else if (snr < 5) color = '#f59e0b'; // amber (-5 to 5 dB)

      const isDirect = link.source === 'direct';
      const isTraceroute = link.source === 'traceroute';

      const line = L.polyline([
        fromPos,
        toPos
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
  const CONFIG_ENUMS = {
    'device.role': ['CLIENT', 'CLIENT_MUTE', 'ROUTER', 'ROUTER_LATE', 'REPEATER', 'TRACKER', 'SENSOR', 'TAK', 'CLIENT_HID', 'LOST_AND_FOUND', 'TAK_TRACKER'],
    'lora.region': ['UNSET', 'US', 'EU_433', 'EU_868', 'CN', 'JP', 'ANZ', 'KR', 'TW', 'RU', 'IN', 'NZ_865', 'TH', 'LORA_24', 'UA_433', 'UA_868', 'MY_433', 'MY_919', 'SG_923', 'PH'],
    'lora.modem_preset': ['LONG_FAST', 'LONG_SLOW', 'VERY_LONG_SLOW', 'MEDIUM_SLOW', 'MEDIUM_FAST', 'SHORT_SLOW', 'SHORT_FAST', 'LONG_MODERATE', 'SHORT_TURBO'],
    'bluetooth.mode': ['FIXED_PIN', 'RANDOM_PIN'],
    'position.gps_mode': ['DISABLED', 'ENABLED', 'NOT_PRESENT'],
    'power.is_power_saving': ['OFF', 'ON'],
    'network.ip_mode': ['DHCP', 'STATIC']
  };

  const SECTION_META = {
    'device': {
      title: 'Device Settings',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>',
      desc: 'Node role, serial debugging, button pin configs, and reboot behaviors'
    },
    'lora': {
      title: 'LoRa Radio',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4.93 19.07a10 10 0 0 1 0-14.14"/><path d="M7.76 16.24a6 6 0 0 1 0-8.48"/><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.48"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14"/></svg>',
      desc: 'Frequency region, modem speed preset, hop limit, transmit power, and bandwidth'
    },
    'position': {
      title: 'Position & GPS',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polygon points="16.24 7.76 14.12 14.12 7.76 16.24 9.88 9.88 16.24 7.76"/></svg>',
      desc: 'GPS broadcast interval, smart positioning, fixed coordinates, and altitude flags'
    },
    'power': {
      title: 'Power Management',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="1" y="6" width="18" height="12" rx="2"/><line x1="23" y1="11" x2="23" y2="13"/></svg>',
      desc: 'Battery ADC multiplier, sleep timeouts, wake schedules, and low power mode'
    },
    'network': {
      title: 'Network & WiFi',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>',
      desc: 'WiFi station credentials, soft AP, NTP time servers, and MQTT gateway'
    },
    'display': {
      title: 'Screen & Display',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>',
      desc: 'Screen timeout, flip 180°, metric vs imperial units, and OLED brightness'
    },
    'bluetooth': {
      title: 'Bluetooth BLE',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5"/></svg>',
      desc: 'BLE advertisement, fixed vs random PIN pairing mode, and connection timeout'
    },
    'security': {
      title: 'Security & Encryption',
      icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>',
      desc: 'Public encryption key, remote admin channel authorization, and lock codes'
    }
  };

  async function loadConfig() {
    if (!state.activeDeviceId && state.devices.length > 0) {
      state.activeDeviceId = state.devices[0].id;
    }
    if (!state.activeDeviceId) return;

    try {
      if (el.configCardsContainer) {
        el.configCardsContainer.innerHTML = `
          <div class="config-loading-state">
            <div class="spinner"></div>
            <span>Fetching configuration from radio...</span>
          </div>
        `;
      }

      const res = await fetch(`/api/config?device=${encodeURIComponent(state.activeDeviceId)}`);
      if (!res.ok) {
        if (el.configCardsContainer) {
          el.configCardsContainer.innerHTML = `
            <div class="config-empty-state">
              <p>Failed to retrieve configuration for this radio.</p>
            </div>
          `;
        }
        return;
      }

      const data = await res.json();
      state.configData = data;
      state.configSections = data.sections || {};
      state.configModified.clear();

      updateConfigSaveButton();
      updateConfigHeaderBadges();
      renderConfigView();
    } catch (err) {
      console.error('Error loading config:', err);
      if (el.configCardsContainer) {
        el.configCardsContainer.innerHTML = `
          <div class="config-empty-state">
            <p>Error connecting to radio configuration service.</p>
          </div>
        `;
      }
    }
  }

  function updateConfigHeaderBadges() {
    const dev = state.devices.find(d => d.id === state.activeDeviceId);
    if (!dev) return;

    if (el.configDeviceTitle) {
      el.configDeviceTitle.textContent = dev.display_name || dev.name || dev.id;
    }

    if (el.configDeviceBadges) {
      const isOnline = !!dev.connected;
      el.configDeviceBadges.innerHTML = `
        <span class="badge ${isOnline ? 'badge-success' : 'badge-muted'}">
          <span class="badge-dot ${isOnline ? 'online' : 'offline'}"></span>
          ${isOnline ? 'Connected' : 'Offline'}
        </span>
        ${dev.hw_model ? `<span class="badge badge-info">${escapeHtml(dev.hw_model)}</span>` : ''}
        ${dev.firmware ? `<span class="badge badge-subtle">FW ${escapeHtml(dev.firmware)}</span>` : ''}
        ${dev.my_node_id ? `<span class="badge badge-subtle mono-val">${escapeHtml(dev.my_node_id)}</span>` : ''}
        <span class="badge badge-subtle mono-val">${escapeHtml(dev.mac || dev.id)}</span>
      `;
    }
  }

  function updateConfigSaveButton() {
    if (!el.btnSaveAllConfig) return;
    const count = state.configModified.size;
    if (count > 0) {
      el.btnSaveAllConfig.disabled = false;
      el.btnSaveAllConfig.innerHTML = `
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path><polyline points="17 21 17 13 7 13 7 21"></polyline><polyline points="7 3 7 8 15 8"></polyline></svg>
        <span>Save Changes (${count})</span>
      `;
      el.btnSaveAllConfig.classList.add('btn-pulse-save');
    } else {
      el.btnSaveAllConfig.disabled = true;
      el.btnSaveAllConfig.innerHTML = `
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path><polyline points="17 21 17 13 7 13 7 21"></polyline><polyline points="7 3 7 8 15 8"></polyline></svg>
        <span>Save Changes</span>
      `;
      el.btnSaveAllConfig.classList.remove('btn-pulse-save');
    }
  }

  function renderConfigView() {
    if (state.activeConfigSection === 'raw') {
      if (el.configCardsContainer) el.configCardsContainer.style.display = 'none';
      if (el.configRawContainer) el.configRawContainer.style.display = 'block';
      renderConfigTable(state.configData?.config || []);
    } else {
      if (el.configCardsContainer) el.configCardsContainer.style.display = 'grid';
      if (el.configRawContainer) el.configRawContainer.style.display = 'none';
      renderConfigCards();
    }
  }

  function renderConfigCards() {
    if (!el.configCardsContainer) return;
    const sections = state.configSections || {};
    const sectionKeys = Object.keys(sections);

    if (sectionKeys.length === 0) {
      el.configCardsContainer.innerHTML = `
        <div class="config-empty-state">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" width="48" height="48"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
          <h3>No Settings Available</h3>
          <p>No configuration packets have been received from this radio yet. Ensure the device is powered and connected, then click Refresh.</p>
          <button class="btn btn-primary" onclick="window.meshApp.refreshConfig()">Refresh from Radio</button>
        </div>
      `;
      return;
    }

    const query = (state.configSearchQuery || '').trim().toLowerCase();
    const activeSection = state.activeConfigSection || 'all';

    // Standard sections vs modules
    const standardSections = ['device', 'lora', 'position', 'power', 'network', 'display', 'bluetooth', 'security'];

    let visibleSectionKeys = sectionKeys;
    if (activeSection === 'modules') {
      visibleSectionKeys = sectionKeys.filter(s => !standardSections.includes(s) || s.startsWith('module_') || s.startsWith('canned_') || s.startsWith('telemetry') || s.startsWith('ambient_') || s.startsWith('mqtt'));
    } else if (activeSection !== 'all') {
      visibleSectionKeys = sectionKeys.filter(s => s === activeSection);
    }

    let cardsHtml = '';
    let totalRenderedItems = 0;

    for (const secKey of visibleSectionKeys) {
      const secData = sections[secKey] || {};
      const fieldKeys = Object.keys(secData);

      // Filter fields by search
      const matchingFieldKeys = fieldKeys.filter(fKey => {
        if (!query) return true;
        const item = secData[fKey];
        const fullKey = item.key || `${secKey}.${fKey}`;
        const valStr = String(item.value || '').toLowerCase();
        return fullKey.toLowerCase().includes(query) || valStr.includes(query) || fKey.toLowerCase().includes(query);
      });

      if (matchingFieldKeys.length === 0) continue;
      totalRenderedItems += matchingFieldKeys.length;

      const meta = SECTION_META[secKey] || {
        title: secKey.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase()),
        icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/></svg>',
        desc: `Configuration parameters for ${secKey}`
      };

      const fieldsHtml = matchingFieldKeys.map(fKey => {
        const item = secData[fKey];
        const fullKey = item.key || `${secKey}.${fKey}`;
        const cleanKey = fullKey.replace(/[^a-zA-Z0-9_-]/g, '_');
        const origVal = item.value != null ? String(item.value) : '';
        const currentVal = state.configModified.has(fullKey) ? state.configModified.get(fullKey) : origVal;
        const isModified = state.configModified.has(fullKey) && state.configModified.get(fullKey) !== origVal;

        let inputControlHtml = '';
        const enums = CONFIG_ENUMS[fullKey];

        if (item.type === 'boolean' || origVal === 'ON' || origVal === 'OFF' || origVal === 'true' || origVal === 'false') {
          const isChecked = currentVal === 'ON' || currentVal === 'true' || currentVal === '1';
          inputControlHtml = `
            <div class="config-field-control">
              <label class="config-toggle-switch">
                <input type="checkbox" id="cfg-ctrl-${cleanKey}" ${isChecked ? 'checked' : ''} onchange="window.meshApp.onToggleChanged('${escapeHtml(fullKey)}', this.checked)">
                <span class="toggle-slider"></span>
              </label>
              <span class="config-toggle-label ${isChecked ? 'is-on' : 'is-off'}">${isChecked ? 'ENABLED' : 'DISABLED'}</span>
            </div>
          `;
        } else if (enums && Array.isArray(enums)) {
          inputControlHtml = `
            <div class="config-field-control">
              <select id="cfg-ctrl-${cleanKey}" class="config-select" onchange="window.meshApp.onInputChanged('${escapeHtml(fullKey)}', this.value)">
                ${enums.map(opt => `
                  <option value="${escapeHtml(opt)}" ${opt.toUpperCase() === currentVal.toUpperCase() ? 'selected' : ''}>
                    ${escapeHtml(opt)}
                  </option>
                `).join('')}
                ${!enums.map(e => e.toUpperCase()).includes(currentVal.toUpperCase()) && currentVal ? `
                  <option value="${escapeHtml(currentVal)}" selected>${escapeHtml(currentVal)} (Custom)</option>
                ` : ''}
              </select>
            </div>
          `;
        } else if (item.type === 'number' || (!isNaN(Number(origVal)) && origVal.trim() !== '')) {
          inputControlHtml = `
            <div class="config-field-control">
              <input type="number" id="cfg-ctrl-${cleanKey}" class="config-input config-input-number" value="${escapeHtml(currentVal)}" oninput="window.meshApp.onInputChanged('${escapeHtml(fullKey)}', this.value)" onkeydown="if(event.key==='Enter') window.meshApp.saveConfigKey('${escapeHtml(fullKey)}')">
            </div>
          `;
        } else {
          // Text / String / Key
          const isPassword = fKey.toLowerCase().includes('psk') || fKey.toLowerCase().includes('pass') || fKey.toLowerCase().includes('key');
          inputControlHtml = `
            <div class="config-field-control">
              <input type="${isPassword ? 'password' : 'text'}" id="cfg-ctrl-${cleanKey}" class="config-input" value="${escapeHtml(currentVal)}" oninput="window.meshApp.onInputChanged('${escapeHtml(fullKey)}', this.value)" onkeydown="if(event.key==='Enter') window.meshApp.saveConfigKey('${escapeHtml(fullKey)}')">
            </div>
          `;
        }

        const friendlyName = fKey.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase());

        return `
          <div class="config-item ${isModified ? 'is-modified' : ''}" id="cfg-item-${cleanKey}">
            <div class="config-item-info">
              <div class="config-item-title-row">
                <span class="config-item-name">${escapeHtml(friendlyName)}</span>
                ${isModified ? '<span class="config-modified-badge">Modified</span>' : ''}
              </div>
              <span class="config-item-key">${escapeHtml(fullKey)}</span>
            </div>
            <div class="config-item-actions">
              ${inputControlHtml}
              <button class="btn btn-sm btn-secondary btn-item-save" title="Save ${escapeHtml(fullKey)} to radio" onclick="window.meshApp.saveConfigKey('${escapeHtml(fullKey)}')">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14"><polyline points="20 6 9 17 4 12"></polyline></svg>
                <span>Save</span>
              </button>
            </div>
          </div>
        `;
      }).join('');

      cardsHtml += `
        <div class="config-card" id="cfg-card-${secKey}">
          <div class="config-card-header">
            <div class="config-card-title-group">
              <div class="config-card-icon">${meta.icon}</div>
              <div>
                <h3 class="config-card-title">${escapeHtml(meta.title)}</h3>
                <span class="config-card-desc">${escapeHtml(meta.desc)}</span>
              </div>
            </div>
            <span class="badge badge-subtle">${matchingFieldKeys.length} ${matchingFieldKeys.length === 1 ? 'item' : 'items'}</span>
          </div>
          <div class="config-card-body">
            ${fieldsHtml}
          </div>
        </div>
      `;
    }

    if (totalRenderedItems === 0) {
      el.configCardsContainer.innerHTML = `
        <div class="config-empty-state">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" width="48" height="48"><circle cx="11" cy="11" r="8"></circle><line x1="21" y1="21" x2="16.65" y2="16.65"></line></svg>
          <h3>No Matching Settings</h3>
          <p>No configuration items matched "${escapeHtml(query)}". Try clearing your search filter.</p>
          <button class="btn btn-secondary" onclick="window.meshApp.clearConfigSearch()">Clear Filter</button>
        </div>
      `;
      return;
    }

    el.configCardsContainer.innerHTML = cardsHtml;
  }

  function renderConfigTable(items) {
    if (!el.configTableBody) return;
    const search = (state.configSearchQuery || '').toLowerCase().trim();

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

    const filtered = normalized.filter(item => !search || item.key.toLowerCase().includes(search) || item.value.toLowerCase().includes(search));

    if (filtered.length === 0) {
      el.configTableBody.innerHTML = `
        <tr>
          <td colspan="3" style="text-align:center; padding: 2rem; color: var(--text-muted);">
            No configuration settings matching "${escapeHtml(search)}".
          </td>
        </tr>
      `;
      return;
    }

    el.configTableBody.innerHTML = filtered.map(({ key, value }) => {
      const cleanKey = key.replace(/[^a-zA-Z0-9_-]/g, '_');
      const curVal = state.configModified.has(key) ? state.configModified.get(key) : value;
      const isModified = state.configModified.has(key) && state.configModified.get(key) !== value;

      return `
        <tr class="${isModified ? 'row-modified' : ''}">
          <td class="config-key">
            <span class="mono-val">${escapeHtml(key)}</span>
            ${isModified ? '<span class="config-modified-badge" style="margin-left:8px;">Modified</span>' : ''}
          </td>
          <td>
            <input type="text" class="config-val-input ${isModified ? 'is-modified' : ''}" id="cfg-raw-${cleanKey}" value="${escapeHtml(curVal)}" oninput="window.meshApp.onInputChanged('${escapeHtml(key)}', this.value)" onkeydown="if(event.key==='Enter') window.meshApp.saveConfigKey('${escapeHtml(key)}')">
          </td>
          <td>
            <button class="btn btn-sm btn-primary" onclick="window.meshApp.saveConfigKey('${escapeHtml(key)}')">Save</button>
          </td>
        </tr>
      `;
    }).join('');
  }

  function handleConfigFieldChange(key, value) {
    // Find original value
    let origVal = '';
    const parts = key.split('.');
    if (parts.length >= 2 && state.configSections && state.configSections[parts[0]] && state.configSections[parts[0]][parts.slice(1).join('.')]) {
      origVal = String(state.configSections[parts[0]][parts.slice(1).join('.')].value || '');
    }

    if (value === origVal) {
      state.configModified.delete(key);
    } else {
      state.configModified.set(key, value);
    }

    // Update item DOM element state
    const cleanKey = key.replace(/[^a-zA-Z0-9_-]/g, '_');
    const itemEl = document.getElementById(`cfg-item-${cleanKey}`);
    if (itemEl) {
      if (state.configModified.has(key)) {
        itemEl.classList.add('is-modified');
        let badge = itemEl.querySelector('.config-modified-badge');
        if (!badge) {
          const titleRow = itemEl.querySelector('.config-item-title-row');
          if (titleRow) {
            titleRow.insertAdjacentHTML('beforeend', '<span class="config-modified-badge">Modified</span>');
          }
        }
      } else {
        itemEl.classList.remove('is-modified');
        const badge = itemEl.querySelector('.config-modified-badge');
        if (badge) badge.remove();
      }
    }

    updateConfigSaveButton();
  }

  async function saveConfigKey(key, customVal) {
    let value = customVal;
    const cleanKey = key.replace(/[^a-zA-Z0-9_-]/g, '_');

    if (value === undefined) {
      if (state.configModified.has(key)) {
        value = state.configModified.get(key);
      } else {
        const cardInput = document.getElementById(`cfg-ctrl-${cleanKey}`);
        const rawInput = document.getElementById(`cfg-raw-${cleanKey}`);
        if (cardInput) {
          value = cardInput.type === 'checkbox' ? (cardInput.checked ? 'ON' : 'OFF') : cardInput.value.trim();
        } else if (rawInput) {
          value = rawInput.value.trim();
        }
      }
    }

    if (value === undefined) return;

    try {
      const itemEl = document.getElementById(`cfg-item-${cleanKey}`);
      const btn = itemEl ? itemEl.querySelector('.btn-item-save') : null;
      if (btn) {
        btn.disabled = true;
        btn.innerHTML = '<span class="spinner-sm"></span>';
      }

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
        state.configModified.delete(key);
        // Update local section cache
        const parts = key.split('.');
        if (parts.length >= 2 && state.configSections && state.configSections[parts[0]] && state.configSections[parts[0]][parts.slice(1).join('.')]) {
          state.configSections[parts[0]][parts.slice(1).join('.')].value = value;
        }

        if (itemEl) {
          itemEl.classList.remove('is-modified');
          itemEl.classList.add('save-success-flash');
          setTimeout(() => itemEl.classList.remove('save-success-flash'), 1200);
          const badge = itemEl.querySelector('.config-modified-badge');
          if (badge) badge.remove();
        }

        if (btn) {
          btn.disabled = false;
          btn.classList.add('btn-success');
          btn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14"><polyline points="20 6 9 17 4 12"></polyline></svg> Saved';
          setTimeout(() => {
            btn.classList.remove('btn-success');
            btn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14"><polyline points="20 6 9 17 4 12"></polyline></svg> <span>Save</span>';
          }, 1500);
        }

        updateConfigSaveButton();
      } else {
        alert(`Failed to update setting "${key}" on radio.`);
        if (btn) {
          btn.disabled = false;
          btn.innerHTML = '<span>Save</span>';
        }
      }
    } catch (err) {
      console.error('Error updating config:', err);
      alert(`Network error saving setting "${key}".`);
    }
  }

  async function saveAllConfig() {
    if (state.configModified.size === 0) return;
    const settings = Object.fromEntries(state.configModified);
    const count = state.configModified.size;

    try {
      if (el.btnSaveAllConfig) {
        el.btnSaveAllConfig.disabled = true;
        el.btnSaveAllConfig.innerHTML = '<span class="spinner-sm"></span> Saving to radio...';
      }

      const res = await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device: state.activeDeviceId,
          settings: settings
        })
      });

      if (res.ok) {
        state.configModified.clear();
        updateConfigSaveButton();
        await loadConfig();
        alert(`Successfully saved ${count} setting${count > 1 ? 's' : ''} to radio.`);
      } else {
        alert('Failed to save settings to radio.');
        updateConfigSaveButton();
      }
    } catch (err) {
      console.error('Error saving all config:', err);
      alert('Network error while saving settings.');
      updateConfigSaveButton();
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

      // Ensure activeDeviceId is selected
      if (!state.activeDeviceId && state.devices.length > 0) {
        const liveDev = state.devices.find(d => d.connected);
        state.activeDeviceId = liveDev ? liveDev.id : state.devices[0].id;
      }

      const cur = state.devices.find(d => d.id === state.activeDeviceId);
      if (cur && cur.my_node_num) {
        state.myNodeNum = cur.my_node_num;
        state.myNodeId = cur.my_node_id || '';
      }

      if (el.deviceSelect) {
        el.deviceSelect.innerHTML = state.devices.map(d => `
          <option value="${escapeHtml(d.id)}" ${d.id === state.activeDeviceId ? 'selected' : ''}>
            ${d.connected ? '● ' : '○ '}${escapeHtml(d.display_name || d.name || d.id)}
          </option>
        `).join('');
      }

      if (el.configDevicesCount) {
        el.configDevicesCount.textContent = `${state.devices.length} ${state.devices.length === 1 ? 'radio' : 'radios'}`;
      }

      if (el.configDevicesList) {
        el.configDevicesList.innerHTML = state.devices.map(d => {
          const isCurrent = d.id === state.activeDeviceId;
          const isLive = !!d.connected;
          const name = d.name || d.display_name || d.id;
          const mac = d.mac || d.id;
          return `
            <li class="config-device-item ${isCurrent ? 'active' : ''}" onclick="window.meshApp.selectDevice('${escapeHtml(d.id)}')">
              <div class="config-device-dot ${isLive ? 'online' : 'offline'}" title="${isLive ? 'Connected' : 'Offline'}"></div>
              <div class="config-device-info">
                <div class="config-device-name">${escapeHtml(name)}</div>
                <div class="config-device-mac">${escapeHtml(mac)}</div>
              </div>
              ${d.hw_model ? `<span class="config-device-badge">${escapeHtml(d.hw_model)}</span>` : ''}
            </li>
          `;
        }).join('');
      }

      updateConfigHeaderBadges();
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

    if (el.statsNodeSelect) {
      const cur = state.statsNodeFilter || 0;
      el.statsNodeSelect.innerHTML = `<option value="0">All Nodes (Global Mesh)</option>` +
        Array.from(state.nodes.values()).map(n => `
          <option value="${n.node_num}" ${n.node_num === cur ? 'selected' : ''}>
            ${escapeHtml(n.long_name || n.short_name)} (${n.node_id})
          </option>
        `).join('');
    }
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

    // Statistics Range, Filter & Search Listeners
    document.querySelectorAll('#stats-range-chips .filter-chip').forEach(chip => {
      chip.addEventListener('click', () => {
        document.querySelectorAll('#stats-range-chips .filter-chip').forEach(c => c.classList.remove('active'));
        chip.classList.add('active');
        state.statsRange = chip.dataset.range || '1d';
        fetchAndRenderStats();
      });
    });

    el.statsNodeSelect?.addEventListener('change', () => {
      state.statsNodeFilter = parseInt(el.statsNodeSelect.value, 10) || 0;
      fetchAndRenderStats();
    });

    el.btnRefreshStats?.addEventListener('click', () => {
      fetchAndRenderStats();
    });

    el.statsTableSearch?.addEventListener('input', () => {
      state.statsSearchQuery = el.statsTableSearch.value;
      if (el.btnStatsSearchClear) {
        el.btnStatsSearchClear.style.display = state.statsSearchQuery ? 'block' : 'none';
      }
      renderStatsTable();
    });

    el.btnStatsSearchClear?.addEventListener('click', () => {
      state.statsSearchQuery = '';
      if (el.statsTableSearch) el.statsTableSearch.value = '';
      el.btnStatsSearchClear.style.display = 'none';
      renderStatsTable();
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

    // Config search & filters
    el.configSearch?.addEventListener('input', () => {
      state.configSearchQuery = el.configSearch.value;
      if (el.btnConfigSearchClear) {
        el.btnConfigSearchClear.style.display = state.configSearchQuery ? 'block' : 'none';
      }
      renderConfigView();
    });

    el.btnConfigSearchClear?.addEventListener('click', () => {
      state.configSearchQuery = '';
      if (el.configSearch) el.configSearch.value = '';
      if (el.btnConfigSearchClear) el.btnConfigSearchClear.style.display = 'none';
      renderConfigView();
    });

    el.configCategoryPills?.addEventListener('click', (e) => {
      const pill = e.target.closest('.config-pill');
      if (!pill) return;
      document.querySelectorAll('#config-category-pills .config-pill').forEach(p => p.classList.remove('active'));
      pill.classList.add('active');
      state.activeConfigSection = pill.dataset.section || 'all';
      renderConfigView();
    });

    el.btnRefreshConfig?.addEventListener('click', () => {
      loadConfig();
    });

    el.btnSaveAllConfig?.addEventListener('click', () => {
      saveAllConfig();
    });
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

  function isLocalNodePacket(packet) {
    if (!packet || !packet.from_node) return false;
    if (state.myNodeNum && packet.from_node === state.myNodeNum) return true;
    if (Array.isArray(state.devices) && state.devices.some(d => d.my_node_num && d.my_node_num === packet.from_node)) return true;
    return false;
  }

  function addPacketToHud(packet, isNew = true) {
    if (!el.packetHudList) return;

    // Exclude local node keepalive, telemetry, and node status packets from Live RF Packet Traffic
    if (isLocalNodePacket(packet) && (
      packet.port_name === 'TELEMETRY_APP' ||
      packet.port_name === 'NODEINFO_APP' ||
      packet.port_name === 'POSITION_APP'
    )) {
      return;
    }

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

    const fromMarker = state.nodeMarkers.get(packet.from_node);
    const toMarker = (packet.to_node && packet.to_node !== 0xFFFFFFFF) ? state.nodeMarkers.get(packet.to_node) : null;

    const coords = [];
    if (fromMarker) {
      coords.push(fromMarker.getLatLng());
    } else if (fromNode && fromNode.latitude && fromNode.longitude) {
      coords.push([fromNode.latitude, fromNode.longitude]);
    }
    if (toMarker) {
      coords.push(toMarker.getLatLng());
    } else if (toNode && toNode.latitude && toNode.longitude) {
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
    if (isLocalNodePacket(packet) && (packet.port_name === 'TELEMETRY_APP' || packet.port_name === 'NODEINFO_APP')) return;

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
        const marker = state.nodeMarkers.get(nodeNum);
        const targetPos = marker ? marker.getLatLng() : [node.latitude, node.longitude];
        map.setView(targetPos, 15);
        selectNodeForMap(node);
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
      if (el.deviceSelect) el.deviceSelect.value = deviceId;
      fetchDevices();
      loadConfig();
    },

    saveConfigKey: saveConfigKey,
    saveAllConfig: saveAllConfig,
    refreshConfig: loadConfig,
    onToggleChanged: function(key, checked) {
      handleConfigFieldChange(key, checked ? 'ON' : 'OFF');
      const cleanKey = key.replace(/[^a-zA-Z0-9_-]/g, '_');
      const itemEl = document.getElementById(`cfg-item-${cleanKey}`);
      if (itemEl) {
        const label = itemEl.querySelector('.config-toggle-label');
        if (label) {
          label.textContent = checked ? 'ENABLED' : 'DISABLED';
          label.className = `config-toggle-label ${checked ? 'is-on' : 'is-off'}`;
        }
      }
    },
    onInputChanged: function(key, value) {
      handleConfigFieldChange(key, value);
    },
    clearConfigSearch: function() {
      state.configSearchQuery = '';
      if (el.configSearch) el.configSearch.value = '';
      if (el.btnConfigSearchClear) el.btnConfigSearchClear.style.display = 'none';
      renderConfigView();
    },

    filterStatsByNode: function(nodeNum) {
      state.statsNodeFilter = nodeNum;
      if (el.statsNodeSelect) el.statsNodeSelect.value = nodeNum;
      const statsTab = document.querySelector('.nav-tab[data-tab="stats"]');
      if (statsTab && !statsTab.classList.contains('active')) {
        statsTab.click();
      } else {
        fetchAndRenderStats();
      }
    },

    clearStatsNodeFilter: function() {
      state.statsNodeFilter = 0;
      if (el.statsNodeSelect) el.statsNodeSelect.value = 0;
      fetchAndRenderStats();
    }
  };

  // ==========================================================================
  // Statistics Dashboard & Charts
  // ==========================================================================
  function initStatsCharts() {
    const chartOptions = {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      plugins: {
        legend: {
          labels: { color: '#94a3b8', font: { family: 'Inter', size: 11 } }
        }
      },
      scales: {
        x: {
          ticks: { color: '#64748b', font: { family: 'Inter', size: 10 } },
          grid: { color: 'rgba(255, 255, 255, 0.05)' }
        },
        y: {
          ticks: { color: '#64748b', font: { family: 'Inter', size: 10 } },
          grid: { color: 'rgba(255, 255, 255, 0.05)' },
          beginAtZero: true
        }
      }
    };

    // 1. Timeline Chart (Stacked Bar)
    const ctxTimeline = document.getElementById('chart-stats-timeline')?.getContext('2d');
    if (ctxTimeline) {
      state.charts.statsTimeline = new Chart(ctxTimeline, {
        type: 'bar',
        data: {
          labels: [],
          datasets: [
            { label: 'Messages', data: [], backgroundColor: '#0ea5e9' },
            { label: 'Telemetry', data: [], backgroundColor: '#f59e0b' },
            { label: 'Positions', data: [], backgroundColor: '#8b5cf6' },
            { label: 'Other/ACKs', data: [], backgroundColor: '#06b6d4' }
          ]
        },
        options: {
          ...chartOptions,
          scales: {
            x: { ...chartOptions.scales.x, stacked: true },
            y: { ...chartOptions.scales.y, stacked: true }
          }
        }
      });
    }

    // 2. Port Distribution Doughnut Chart
    const ctxDoughnut = document.getElementById('chart-stats-doughnut')?.getContext('2d');
    if (ctxDoughnut) {
      state.charts.statsDoughnut = new Chart(ctxDoughnut, {
        type: 'doughnut',
        data: {
          labels: [],
          datasets: [{
            data: [],
            backgroundColor: [
              '#0ea5e9', // text
              '#f59e0b', // telemetry
              '#8b5cf6', // position
              '#10b981', // routing/ack
              '#ec4899', // traceroute
              '#3b82f6', // nodeinfo
              '#64748b'  // other
            ],
            borderColor: '#111827',
            borderWidth: 2
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          cutout: '65%',
          plugins: {
            legend: {
              position: 'right',
              labels: { color: '#94a3b8', font: { family: 'Inter', size: 11 }, boxWidth: 12 }
            }
          }
        }
      });
    }

    // 3. Top Talkers Horizontal Bar Chart
    const ctxNodes = document.getElementById('chart-stats-nodes')?.getContext('2d');
    if (ctxNodes) {
      state.charts.statsNodes = new Chart(ctxNodes, {
        type: 'bar',
        data: {
          labels: [],
          datasets: [{
            label: 'Packets Transmitted',
            data: [],
            backgroundColor: '#06b6d4',
            borderRadius: 4
          }]
        },
        options: {
          ...chartOptions,
          indexAxis: 'y',
          plugins: {
            legend: { display: false }
          }
        }
      });
    }
  }

  async function fetchAndRenderStats() {
    const range = state.statsRange || '1d';
    const nodeFilter = state.statsNodeFilter || 0;
    const url = `/api/stats?range=${encodeURIComponent(range)}${nodeFilter ? `&node=${nodeFilter}` : ''}`;

    try {
      const res = await fetch(url);
      if (!res.ok) return;
      const data = await res.json();
      state.statsData = data;

      // 1. KPI Cards
      const m = data.metrics || {};
      if (el.statsTotalPackets) el.statsTotalPackets.textContent = (m.total_packets || 0).toLocaleString();
      if (el.statsCastRatio) {
        const b = (m.broadcast_count || 0).toLocaleString();
        const u = (m.unicast_count || 0).toLocaleString();
        el.statsCastRatio.textContent = `${b} broadcast / ${u} unicast`;
      }
      if (el.statsActiveNodes) el.statsActiveNodes.textContent = (m.active_nodes || 0).toLocaleString();
      if (el.statsMsgCount) el.statsMsgCount.textContent = (m.msg_count || 0).toLocaleString();
      if (el.statsTelemetryCount) el.statsTelemetryCount.textContent = (m.telemetry_count || 0).toLocaleString();
      if (el.statsPosCount) el.statsPosCount.textContent = (m.position_count || 0).toLocaleString();
      if (el.statsAvgSnr) {
        el.statsAvgSnr.textContent = (m.avg_snr != null && m.avg_snr !== 0) ? `${m.avg_snr.toFixed(1)} dB` : '-- dB';
      }

      // Resolution badge
      if (el.statsTimelineResolution) {
        let resText = '30m buckets';
        if (range === '1h') resText = '2m buckets';
        else if (range === '6h') resText = '10m buckets';
        else if (range === '1d') resText = '30m buckets';
        else if (range === '7d') resText = '2h buckets';
        else if (range === 'all') resText = '1d buckets';
        el.statsTimelineResolution.textContent = resText;
      }

      // 2. Timeline Chart
      const timeline = data.timeline || [];
      if (state.charts.statsTimeline) {
        if (timeline.length === 0) {
          if (el.overlayStatsTimeline) el.overlayStatsTimeline.style.display = 'flex';
          state.charts.statsTimeline.data.labels = [];
          state.charts.statsTimeline.data.datasets.forEach(ds => ds.data = []);
        } else {
          if (el.overlayStatsTimeline) el.overlayStatsTimeline.style.display = 'none';
          const labels = timeline.map(b => {
            const d = new Date(b.ts * 1000);
            if (range === '1h' || range === '6h' || range === '1d') {
              return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
            } else {
              return `${d.getMonth() + 1}/${d.getDate()} ${d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
            }
          });
          state.charts.statsTimeline.data.labels = labels;
          state.charts.statsTimeline.data.datasets[0].data = timeline.map(b => b.msg_count);
          state.charts.statsTimeline.data.datasets[1].data = timeline.map(b => b.telemetry_count);
          state.charts.statsTimeline.data.datasets[2].data = timeline.map(b => b.pos_count);
          state.charts.statsTimeline.data.datasets[3].data = timeline.map(b => b.other_count);
        }
        state.charts.statsTimeline.update();
      }

      // 3. Port Distribution Doughnut Chart
      const ports = data.ports || {};
      const portEntries = Object.entries(ports).filter(([_, cnt]) => cnt > 0);
      if (state.charts.statsDoughnut) {
        if (portEntries.length === 0) {
          if (el.overlayStatsDoughnut) el.overlayStatsDoughnut.style.display = 'flex';
          state.charts.statsDoughnut.data.labels = [];
          state.charts.statsDoughnut.data.datasets[0].data = [];
        } else {
          if (el.overlayStatsDoughnut) el.overlayStatsDoughnut.style.display = 'none';
          const prettyPortName = (p) => {
            if (p === 'TEXT_MESSAGE_APP') return 'Messages';
            if (p === 'TELEMETRY_APP') return 'Telemetry';
            if (p === 'POSITION_APP') return 'Position';
            if (p === 'ROUTING_APP') return 'ACK/Routing';
            if (p === 'TRACEROUTE_APP') return 'Traceroute';
            if (p === 'NODEINFO_APP') return 'NodeInfo';
            return p;
          };
          state.charts.statsDoughnut.data.labels = portEntries.map(([p]) => prettyPortName(p));
          state.charts.statsDoughnut.data.datasets[0].data = portEntries.map(([_, cnt]) => cnt);
        }
        state.charts.statsDoughnut.update();
      }

      // 4. Top Talkers Bar Chart
      const nodes = data.nodes || [];
      const topNodes = [...nodes].slice(0, 8); // Top 8 by packet count
      if (state.charts.statsNodes) {
        if (topNodes.length === 0) {
          if (el.overlayStatsNodes) el.overlayStatsNodes.style.display = 'flex';
          state.charts.statsNodes.data.labels = [];
          state.charts.statsNodes.data.datasets[0].data = [];
        } else {
          if (el.overlayStatsNodes) el.overlayStatsNodes.style.display = 'none';
          state.charts.statsNodes.data.labels = topNodes.map(n => n.short_name || n.long_name || n.node_id);
          state.charts.statsNodes.data.datasets[0].data = topNodes.map(n => n.packet_count);
        }
        state.charts.statsNodes.update();
      }

      // 5. Per-Node Table
      renderStatsTable();

    } catch (err) {
      console.error('Error fetching stats:', err);
    }
  }

  function renderStatsTable() {
    if (!el.statsTableBody) return;
    const nodes = (state.statsData && state.statsData.nodes) || [];
    const q = (state.statsSearchQuery || '').toLowerCase().trim();

    const filtered = nodes.filter(n => {
      if (!q) return true;
      return (n.long_name && n.long_name.toLowerCase().includes(q)) ||
             (n.short_name && n.short_name.toLowerCase().includes(q)) ||
             (n.node_id && n.node_id.toLowerCase().includes(q)) ||
             (n.hw_model && n.hw_model.toLowerCase().includes(q)) ||
             (n.role && n.role.toLowerCase().includes(q));
    });

    if (el.statsTableCount) {
      el.statsTableCount.textContent = `Showing ${filtered.length} of ${nodes.length} nodes`;
    }

    if (filtered.length === 0) {
      el.statsTableBody.innerHTML = `
        <tr>
          <td colspan="10" style="text-align:center; padding: 2rem; color: var(--text-muted);">
            No node traffic recorded matching criteria.
          </td>
        </tr>
      `;
      return;
    }

    el.statsTableBody.innerHTML = filtered.map(n => {
      const isFiltered = state.statsNodeFilter === n.node_num;
      const myBadge = n.is_local ? '<span class="my-node-badge" style="margin-left:4px;">ME</span>' : '';
      const name = escapeHtml(n.long_name || n.short_name || n.node_id);
      const shortBadge = n.short_name ? `<span class="badge" style="background:var(--bg-elevated); border:1px solid var(--border-subtle); color:var(--text-secondary);">${escapeHtml(n.short_name)}</span>` : '';
      const hwModel = escapeHtml(n.hw_model || 'Unknown HW');
      const role = escapeHtml(n.role || 'CLIENT');
      const snrText = (n.avg_snr != null && n.avg_snr !== 0) ? `${n.avg_snr.toFixed(1)} dB` : '--';
      const lastSeen = formatTimeAgo(n.last_seen);

      return `
        <tr style="${isFiltered ? 'background: rgba(6, 182, 212, 0.08);' : ''}">
          <td>
            <div class="node-identity">
              <div class="node-name">${name} ${shortBadge} ${myBadge}</div>
              <span class="node-id-mono">${n.node_id}</span>
            </div>
          </td>
          <td>
            <div>${hwModel}</div>
            <div style="font-size:0.75rem; color:var(--text-muted);">${role}</div>
          </td>
          <td class="mono-val" style="color:var(--accent-cyan); font-size:0.95rem;">${n.packet_count.toLocaleString()}</td>
          <td class="mono-val">${n.msg_count.toLocaleString()}</td>
          <td class="mono-val">${n.telemetry_count.toLocaleString()}</td>
          <td class="mono-val">${n.pos_count.toLocaleString()}</td>
          <td class="mono-val">${n.ack_count.toLocaleString()}</td>
          <td class="mono-val" style="color:#38bdf8;">${snrText}</td>
          <td class="time-col">${lastSeen}</td>
          <td>
            ${isFiltered ? `
              <button class="btn btn-sm btn-secondary" onclick="window.meshApp.clearStatsNodeFilter()">Reset</button>
            ` : `
              <button class="btn btn-sm btn-primary" onclick="window.meshApp.filterStatsByNode(${n.node_num})">Filter</button>
            `}
          </td>
        </tr>
      `;
    }).join('');
  }

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
