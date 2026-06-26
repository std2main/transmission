/* @license This file Copyright © Mnemosyne LLC.
   It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
   or any future license endorsed by Mnemosyne LLC.
   License text can be found in the licenses/ folder. */

import { makeUUID, setEnabled } from './utils.js';
import { Formatter } from './formatter.js';

export class QBittorrentMigrationDialog extends EventTarget {
  constructor(controller, remote) {
    super();

    this.controller = controller;
    this.remote = remote;
    this.closed = false;

    // Load config from localStorage
    this._loadConfig();

    // Active connection state
    this.qbTorrents = [];
    this.qbTrackers = {}; // hash -> trackers array
    this.transferStates = {}; // hash -> status string (e.g., 'Idle', 'Exporting...', 'Verifying...', etc.)
    this.manualOverrides = {}; // hash -> path string
    this.connectionStatus = 'disconnected'; // 'connected', 'connecting', 'disconnected', 'error'
    this.connectionError = '';
    this.qbResolvedUrl = '';

    // Create the DOM layout
    this.elements = this._create();

    // Event listeners
    this.elements.dismiss.addEventListener('click', () => this.close());

    // Append to main window workarea
    const workarea = document.querySelector('#mainwin-workarea');
    if (workarea) {
      workarea.append(this.elements.root);
    } else {
      document.body.append(this.elements.root);
    }

    // Initial load
    this.activeEndpointId = this.config.activeEndpointId || '';
    this._onActiveEndpointChanged();

    // Auto-refresh Transmission stats to track verify progress
    this.refreshInterval = setInterval(() => {
      if (this.connectionStatus === 'connected') {
        this.refreshTransmission();
      }
    }, 4000);
  }

  close() {
    if (!this.closed) {
      clearInterval(this.refreshInterval);
      this.elements.root.remove();
      this.dispatchEvent(new Event('close'));
      for (const key of Object.keys(this)) {
        delete this[key];
      }
      this.closed = true;
    }
  }

  // Load configuration from localStorage
  _loadConfig() {
    const defaultData = {
      activeEndpointId: '',
      autoPauseQb: true,
      autoTagQb: true,
      endpoints: [],
      qbTag: 'transferred',
    };
    try {
      const dataStr = localStorage.getItem('transmission_qb_migration_config');
      this.config = dataStr ? JSON.parse(dataStr) : defaultData;
    } catch (error) {
      console.error('Failed to load migration config', error);
      this.config = defaultData;
    }
  }

  // Save configuration to localStorage
  _saveConfig() {
    try {
      localStorage.setItem(
        'transmission_qb_migration_config',
        JSON.stringify(this.config),
      );
    } catch (error) {
      console.error('Failed to save migration config', error);
    }
  }

  // Get active endpoint configuration
  _getActiveEndpoint() {
    return this.config.endpoints.find((ep) => ep.id === this.activeEndpointId);
  }

  // Mappings helper: maps qB save_path to Transmission download_dir
  _resolvePath(qbSavePath) {
    const endpoint = this._getActiveEndpoint();
    if (!endpoint || !endpoint.mappings || endpoint.mappings.length === 0) {
      return qbSavePath;
    }

    // Match top-to-bottom of mappings list (stable priority)
    for (const rule of endpoint.mappings) {
      if (!rule.source || !rule.target) {
        continue;
      }

      // Ensure slash termination handling to avoid partial directory matches
      const srcBase = rule.source.endsWith('/')
        ? rule.source
        : `${rule.source}/`;
      const targetBase = rule.target.endsWith('/')
        ? rule.target
        : `${rule.target}/`;
      const testPath = qbSavePath.endsWith('/') ? qbSavePath : `${qbSavePath}/`;

      if (testPath.startsWith(srcBase)) {
        const relative = testPath.slice(srcBase.length);
        let resolved = targetBase + relative;
        // Trim trailing slash if original didn't have one
        if (!qbSavePath.endsWith('/') && resolved.endsWith('/')) {
          resolved = resolved.slice(0, -1);
        }
        return resolved;
      }
    }

    return null; // Path resolved to null if no mapping matches
  }

  // Check Transferred State of a torrent
  _getTorrentState(qbTor) {
    const hash = qbTor.hash.toLowerCase();

    // Find matching Transmission torrent
    const trTor =
      this.controller._torrents[hash] ||
      Object.values(this.controller._torrents).find(
        (t) => t.getHashString().toLowerCase() === hash,
      );

    const isMissingFiles =
      qbTor.state === 'missingFiles' ||
      qbTor.state === 'error' ||
      qbTor.progress < 1;

    if (isMissingFiles) {
      return {
        reason: 'Missing Files in qB',
        status: 'untransferable',
        trTor: trTor || null,
      };
    }

    if (!trTor) {
      // Not added to Transmission
      const hasPath =
        this.manualOverrides[hash] || this._resolvePath(qbTor.save_path);
      const hasMeta = qbTor.size > 0;
      let reason = 'Ready';
      if (!hasMeta) {
        reason = 'Missing Metadata';
      } else if (!hasPath) {
        reason = 'No Path Map';
      }
      return {
        reason,
        status: hasMeta && hasPath ? 'transferable' : 'untransferable',
        trTor: null,
      };
    }

    // Check Path Match
    const resolvedPath =
      this.manualOverrides[hash] || this._resolvePath(qbTor.save_path);
    const trPath = trTor.getDownloadDir();
    // Normalize paths by removing trailing slash for comparison
    const normTrPath = trPath.replace(/\/$/, '');
    const normResolvedPath = resolvedPath
      ? resolvedPath.replace(/\/$/, '')
      : '';
    const pathMatched = resolvedPath && normTrPath === normResolvedPath;

    // Check Quick Check (100% progress + no error)
    const checkPassed = trTor.getPercentDone() === 1 && trTor.getError() === 0;

    // Check Trackers Copied
    const trTrackers = new Set(
      trTor.getTrackers().map((t) => t.announce.toLowerCase()),
    );
    const qbTrs = this.qbTrackers[hash] || [];
    const trackersCopied = qbTrs.every((qbt) => {
      // Filter out DHT/PEX/LSD pseudo-trackers
      if (!qbt.url || qbt.url.startsWith('**')) {
        return true;
      }
      return trTrackers.has(qbt.url.toLowerCase());
    });

    if (pathMatched && checkPassed && trackersCopied) {
      return { reason: 'Completed', status: 'transferred', trTor };
    }

    // Mismatches or incomplete state
    let reason = 'Mismatch';
    if (!pathMatched) {
      reason = 'Path Mismatch';
    } else if (!checkPassed) {
      reason = trTor.isChecking() ? 'Verifying...' : 'Checking Failed';
    } else if (!trackersCopied) {
      reason = 'Trackers Mismatch';
    }

    const hasMeta = qbTor.size > 0;
    return {
      reason,
      status: hasMeta ? 'transferable' : 'untransferable',
      trTor,
    };
  }

  // Change active endpoint
  _onActiveEndpointChanged() {
    this.config.activeEndpointId = this.activeEndpointId;
    this._saveConfig();
    this.qbTorrents = [];
    this.qbTrackers = {};
    this.transferStates = {};
    this.manualOverrides = {};
    if (this.elements) {
      this.elements.qbInitialized = false;
    }

    const endpoint = this._getActiveEndpoint();
    if (!endpoint) {
      this.connectionStatus = 'disconnected';
      this._updateConnectionIndicator();
      this._renderQBPane();
      return;
    }

    this.connectQBittorrent();
  }

  // Login and fetch data from qBittorrent
  async connectQBittorrent() {
    const endpoint = this._getActiveEndpoint();
    if (!endpoint) {
      return;
    }

    this.connectionStatus = 'connecting';
    this.connectionError = '';
    this._updateConnectionIndicator();

    try {
      const loginParams = new URLSearchParams();
      loginParams.append('username', endpoint.username);
      loginParams.append('password', endpoint.password);

      // Try connecting directly first
      let targetUrl = endpoint.url;
      let loginRes = null;
      try {
        loginRes = await fetch(`${targetUrl}/api/v2/auth/login`, {
          body: loginParams,
          credentials: 'include',
          method: 'POST',
        });
      } catch (error) {
        // If it looks like a CORS or network error on a cross-origin request, retry via Nginx proxy /qb-api
        const isCrossOrigin =
          targetUrl.startsWith('http') &&
          !targetUrl.startsWith(globalThis.location.origin);
        if (isCrossOrigin) {
          console.warn(
            'Direct connection failed (CORS or network error). Retrying via Nginx proxy /qb-api...',
            error,
          );
          targetUrl = '/qb-api';
          loginRes = await fetch(`${targetUrl}/api/v2/auth/login`, {
            body: loginParams,
            credentials: 'include',
            method: 'POST',
          });
        } else {
          throw error;
        }
      }

      if (!loginRes.ok) {
        throw new Error(`Login HTTP error: ${loginRes.status}`);
      }

      const loginText = await loginRes.text();
      if (!loginText.includes('Ok.')) {
        throw new Error('Invalid credentials');
      }

      // Save the resolved URL (could be direct or the '/qb-api' proxy)
      this.qbResolvedUrl = targetUrl;

      // 2. Fetch torrents list
      const infoRes = await fetch(
        `${this.qbResolvedUrl}/api/v2/torrents/info`,
        {
          credentials: 'include',
        },
      );
      if (!infoRes.ok) {
        throw new Error(`Info HTTP error: ${infoRes.status}`);
      }

      this.qbTorrents = await infoRes.json();
      this.connectionStatus = 'connected';
      this._updateConnectionIndicator();

      // Render the pane
      this._renderQBPane();

      // Proactively fetch trackers for first 30 torrents to quicken the check
      this.fetchSomeTrackers(this.qbTorrents.slice(0, 30));
    } catch (error) {
      console.error(error);
      this.connectionStatus = 'error';
      this.connectionError = error.message || 'Connection failed';
      this._updateConnectionIndicator();
      this._renderQBPane();
    }
  }

  async fetchSomeTrackers(tors) {
    const endpoint = this._getActiveEndpoint();
    if (!endpoint || this.connectionStatus !== 'connected') {
      return;
    }

    for (const t of tors) {
      const hash = t.hash.toLowerCase();
      if (this.qbTrackers[hash]) {
        continue;
      }
      try {
        const res = await fetch(
          `${this.qbResolvedUrl || endpoint.url}/api/v2/torrents/trackers?hash=${hash}`,
          {
            credentials: 'include',
          },
        );
        if (res.ok) {
          const trackers = await res.json();
          this.qbTrackers[hash] = trackers;
          // Trigger local rerender of this torrent row
          this._updateQBRow(hash);
        }
      } catch (error) {
        console.error('Failed to get trackers', error);
      }
    }
  }

  async fetchTrackers(hash) {
    const endpoint = this._getActiveEndpoint();
    if (!endpoint) {
      return [];
    }
    try {
      const res = await fetch(
        `${this.qbResolvedUrl || endpoint.url}/api/v2/torrents/trackers?hash=${hash}`,
        {
          credentials: 'include',
        },
      );
      if (res.ok) {
        const trackers = await res.json();
        this.qbTrackers[hash] = trackers;
        return trackers;
      }
    } catch (error) {
      console.error('Failed to get trackers', error);
    }
    return [];
  }

  // Refresh Transmission list
  refreshTransmission() {
    this.controller._initializeTorrents();
    setTimeout(() => {
      if (this.closed || !this.elements) {
        return;
      }
      this._renderQBPane();
    }, 500);
  }

  // Refresh both
  refreshAll() {
    this.refreshTransmission();
    if (this._getActiveEndpoint()) {
      this.connectQBittorrent();
    }
  }

  // Perform migration for single torrent
  async migrateTorrent(qbTor) {
    const hash = qbTor.hash.toLowerCase();
    const endpoint = this._getActiveEndpoint();
    if (!endpoint) {
      return;
    }

    this.transferStates[hash] = 'Starting...';
    this._updateQBRow(hash);

    try {
      // 1. Resolve path
      const destDir =
        this.manualOverrides[hash] || this._resolvePath(qbTor.save_path);
      if (!destDir) {
        throw new Error(
          'Destination path could not be mapped. Set mapping or manual override.',
        );
      }

      // 2. Export torrent binary
      this.transferStates[hash] = 'Exporting torrent...';
      this._updateQBRow(hash);

      const exportRes = await fetch(
        `${this.qbResolvedUrl || endpoint.url}/api/v2/torrents/export?hash=${hash}`,
        {
          credentials: 'include',
        },
      );
      if (!exportRes.ok) {
        throw new Error(
          `Failed to export torrent from qBittorrent: ${exportRes.status}`,
        );
      }
      const blob = await exportRes.blob();

      const base64Content = await new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onloadend = () => {
          const res = reader.result;
          resolve(res.split('base64,')[1]);
        };
        reader.addEventListener('error', reject);
        reader.readAsDataURL(blob);
      });

      // 3. Add to Transmission in quick check mode (seed_existing_mode: true)
      this.transferStates[hash] = 'Adding to Transmission...';
      this._updateQBRow(hash);

      const addParams = {
        download_dir: destDir,
        metainfo: base64Content,
        paused: true, // Add as paused to prevent auto-seeding/auto-downloading
        seed_existing_mode: true,
      };

      const addResponse = await new Promise((resolve) => {
        this.remote.sendRequest(
          {
            id: 'qb_migration',
            jsonrpc: '2.0',
            method: 'torrent_add',
            params: addParams,
          },
          resolve,
        );
      });

      if ('error' in addResponse) {
        const errStr =
          addResponse.error.data?.errorString ?? addResponse.error.message;
        throw new Error(`Transmission Error: ${errStr}`);
      }

      // 4. Fetch trackers from qB if not cached, then add to Transmission
      this.transferStates[hash] = 'Copying trackers...';
      this._updateQBRow(hash);

      let qbTrs = this.qbTrackers[hash];
      if (!qbTrs) {
        qbTrs = await this.fetchTrackers(hash);
      }

      // Format tracker list
      const announceUrls = qbTrs
        .map((t) => t.url)
        .filter((url) => url && !url.startsWith('**'));

      if (announceUrls.length > 0) {
        // Group by tier or format announce URLs, one per line
        const trackerListStr = announceUrls.join('\n');
        await new Promise((resolve) => {
          this.remote.sendRequest(
            {
              id: 'qb_migration_trackers',
              jsonrpc: '2.0',
              method: 'torrent_set',
              params: {
                ids: [hash],
                tracker_list: trackerListStr,
              },
            },
            resolve,
          );
        });
      }

      // 5. Safe writebacks in qB
      if (this.config.autoTagQb && this.config.qbTag) {
        this.transferStates[hash] = 'Tagging in qB...';
        this._updateQBRow(hash);
        const tagParams = new URLSearchParams();
        tagParams.append('hashes', hash);
        tagParams.append('tags', this.config.qbTag);
        await fetch(
          `${this.qbResolvedUrl || endpoint.url}/api/v2/torrents/addTags`,
          {
            body: tagParams,
            credentials: 'include',
            method: 'POST',
          },
        );
      }

      if (this.config.autoPauseQb) {
        this.transferStates[hash] = 'Pausing in qB...';
        this._updateQBRow(hash);
        const pauseParams = new URLSearchParams();
        pauseParams.append('hashes', hash);
        await fetch(
          `${this.qbResolvedUrl || endpoint.url}/api/v2/torrents/pause`,
          {
            body: pauseParams,
            credentials: 'include',
            method: 'POST',
          },
        );
      }

      this.transferStates[hash] = 'Verifying check...';
      this._updateQBRow(hash);

      // Force refresh Transmission to see new torrent
      this.refreshTransmission();

      // Clear status after brief success notice
      setTimeout(() => {
        delete this.transferStates[hash];
        this._renderQBPane();
      }, 5000);
    } catch (error) {
      console.error(qbTor.name, error);
      this.transferStates[hash] = `Error: ${error.message}`;
      this._updateQBRow(hash);
    }
  }

  // Migrate all transferable torrents
  async migrateAllTransferable() {
    const endpoint = this._getActiveEndpoint();
    if (!endpoint || this.connectionStatus !== 'connected') {
      return;
    }

    const transferable = this.qbTorrents.filter((t) => {
      const state = this._getTorrentState(t);
      return (
        state.status === 'transferable' &&
        !this.transferStates[t.hash.toLowerCase()]
      );
    });

    if (transferable.length === 0) {
      alert('No transferable torrents found.');
      return;
    }

    if (
      confirm(
        `Are you sure you want to migrate ${transferable.length} torrents?`,
      )
    ) {
      for (const t of transferable) {
        // Run sequentially to avoid overloading the browser / Transmission RPC
        await this.migrateTorrent(t);
      }
    }
  }

  // Create UI Structure
  _create() {
    const root = document.createElement('div');
    root.id = 'qb-migration-sidebar';
    root.className = 'qb-migration-sidebar';

    const header = document.createElement('div');
    header.className = 'qb-sidebar-header';
    root.append(header);

    const title = document.createElement('h2');
    title.textContent = 'qBittorrent Migration';
    header.append(title);

    const closeBtn = document.createElement('button');
    closeBtn.className = 'qb-sidebar-close';
    closeBtn.innerHTML = '&times;';
    closeBtn.title = 'Close Sidebar';
    header.append(closeBtn);

    const body = document.createElement('div');
    body.className = 'qb-sidebar-body';
    root.append(body);

    const elements = {
      body,
      dismiss: closeBtn,
      root,
    };
    this.elements = elements;

    // 1. Config Bar Container
    const configBar = document.createElement('div');
    configBar.className = 'qb-config-bar';
    body.append(configBar);

    // Endpoint selector
    const epLabel = document.createElement('label');
    epLabel.textContent = 'Endpoint:';
    configBar.append(epLabel);

    const epSelect = document.createElement('select');
    epSelect.className = 'qb-ep-select';
    epSelect.addEventListener('change', (e) => {
      this.activeEndpointId = e.target.value;
      this._onActiveEndpointChanged();
    });
    configBar.append(epSelect);
    elements.epSelect = epSelect;
    this._renderEndpointOptions();

    // Manage endpoints button
    const manageEpBtn = document.createElement('button');
    manageEpBtn.textContent = 'Connections';
    manageEpBtn.className = 'qb-btn qb-btn-secondary';
    manageEpBtn.addEventListener('click', () => {
      elements.mappingsPanel.classList.add('hidden');
      elements.endpointsPanel.classList.toggle('hidden');
    });
    configBar.append(manageEpBtn);

    // Manage mappings button
    const manageMapsBtn = document.createElement('button');
    manageMapsBtn.textContent = 'Path Map';
    manageMapsBtn.className = 'qb-btn qb-btn-secondary';
    manageMapsBtn.addEventListener('click', () => {
      elements.endpointsPanel.classList.add('hidden');
      elements.mappingsPanel.classList.toggle('hidden');
    });
    configBar.append(manageMapsBtn);

    // Indicator
    const indicator = document.createElement('span');
    indicator.className = 'qb-indicator status-disconnected';
    indicator.textContent = 'Disconnected';
    configBar.append(indicator);
    elements.indicator = indicator;

    // 2. Hidden Editors Area
    const editorsArea = document.createElement('div');
    editorsArea.className = 'qb-editors-area';
    body.append(editorsArea);

    // Connections Panel
    const endpointsPanel = document.createElement('div');
    endpointsPanel.className = 'qb-panel qb-endpoints-panel hidden';
    editorsArea.append(endpointsPanel);
    elements.endpointsPanel = endpointsPanel;
    this._renderEndpointsEditor();

    // Mappings Panel
    const mappingsPanel = document.createElement('div');
    mappingsPanel.className = 'qb-panel qb-mappings-panel hidden';
    editorsArea.append(mappingsPanel);
    elements.mappingsPanel = mappingsPanel;
    this._renderMappingsEditor();

    // 3. qBittorrent Pane
    const qbPane = document.createElement('div');
    qbPane.className = 'qb-pane qb-pane-qb';
    body.append(qbPane);
    elements.qbPane = qbPane;
    this._renderQBPane();

    return elements;
  }

  // Populate endpoints select options
  _renderEndpointOptions() {
    if (!this.elements || !this.elements.epSelect) {
      return;
    }
    const select = this.elements.epSelect;
    select.innerHTML = '';

    const noneOpt = document.createElement('option');
    noneOpt.value = '';
    noneOpt.textContent = '-- Select qBittorrent --';
    select.append(noneOpt);

    for (const ep of this.config.endpoints) {
      const opt = document.createElement('option');
      opt.value = ep.id;
      opt.textContent = ep.name;
      opt.selected = ep.id === this.activeEndpointId;
      select.append(opt);
    }
  }

  // Render endpoints list and editing controls
  _renderEndpointsEditor() {
    const panel = this.elements.endpointsPanel;
    panel.innerHTML = '<h3>qBittorrent Connection Settings</h3>';

    // List existing
    const list = document.createElement('div');
    list.className = 'qb-config-list';
    panel.append(list);

    if (this.config.endpoints.length === 0) {
      list.innerHTML = '<p class="qb-empty">No connections configured.</p>';
    } else {
      for (const ep of this.config.endpoints) {
        const item = document.createElement('div');
        item.className = 'qb-config-item';

        const info = document.createElement('span');
        info.innerHTML = `<strong>${ep.name}</strong> (${ep.url})`;
        item.append(info);

        const actions = document.createElement('div');
        item.append(actions);

        const delBtn = document.createElement('button');
        delBtn.textContent = 'Delete';
        delBtn.className = 'qb-btn qb-btn-danger';
        delBtn.addEventListener('click', () => {
          this.config.endpoints = this.config.endpoints.filter(
            (e) => e.id !== ep.id,
          );
          if (this.activeEndpointId === ep.id) {
            this.activeEndpointId = '';
          }
          this._saveConfig();
          this._renderEndpointOptions();
          this._renderEndpointsEditor();
          this._onActiveEndpointChanged();
        });
        actions.append(delBtn);

        list.append(item);
      }
    }

    // Add new form
    const form = document.createElement('div');
    form.className = 'qb-add-form';
    panel.append(form);

    form.innerHTML = `
      <h4>Add Connection</h4>
      <div class="qb-form-row">
        <label>Name:</label>
        <input type="text" placeholder="Local qBittorrent" id="qb-new-name">
      </div>
      <div class="qb-form-row">
        <label>URL:</label>
        <input type="text" placeholder="http://localhost:8080" id="qb-new-url">
      </div>
      <div class="qb-form-row">
        <label>Username:</label>
        <input type="text" placeholder="admin" id="qb-new-user">
      </div>
      <div class="qb-form-row">
        <label>Password:</label>
        <input type="password" id="qb-new-pass">
      </div>
    `;

    // Writeback settings
    const optsRow = document.createElement('div');
    optsRow.className = 'qb-opts-row';
    form.append(optsRow);

    const pauseLabel = document.createElement('label');
    const pauseCheck = document.createElement('input');
    pauseCheck.type = 'checkbox';
    pauseCheck.checked = this.config.autoPauseQb;
    pauseCheck.addEventListener('change', (e) => {
      this.config.autoPauseQb = e.target.checked;
      this._saveConfig();
    });
    pauseLabel.append(pauseCheck, ' Auto Pause in qB on successful transfer');
    optsRow.append(pauseLabel);

    optsRow.append(document.createElement('br'));

    const tagLabel = document.createElement('label');
    const tagCheck = document.createElement('input');
    tagCheck.type = 'checkbox';
    tagCheck.checked = this.config.autoTagQb;
    tagCheck.addEventListener('change', (e) => {
      this.config.autoTagQb = e.target.checked;
      this._saveConfig();
    });
    tagLabel.append(tagCheck, ' Tag in qB on successful transfer (tag: ');

    const tagInput = document.createElement('input');
    tagInput.type = 'text';
    tagInput.value = this.config.qbTag;
    tagInput.style.width = '80px';
    tagInput.addEventListener('change', (e) => {
      this.config.qbTag = e.target.value.trim();
      this._saveConfig();
    });
    tagLabel.append(tagInput, ')');
    optsRow.append(tagLabel);

    const saveBtn = document.createElement('button');
    saveBtn.textContent = 'Add Connection';
    saveBtn.className = 'qb-btn qb-btn-primary';
    saveBtn.addEventListener('click', () => {
      const name = document.querySelector('#qb-new-name').value.trim();
      const url = document
        .querySelector('#qb-new-url')
        .value.trim()
        .replace(/\/$/, '');
      const user = document.querySelector('#qb-new-user').value.trim();
      const pass = document.querySelector('#qb-new-pass').value;

      if (!name || !url || !user || !pass) {
        alert('All fields are required.');
        return;
      }

      const newEp = {
        id: makeUUID(),
        mappings: [],
        name,
        password: pass,
        url,
        username: user,
      };

      this.config.endpoints.push(newEp);
      if (!this.activeEndpointId) {
        this.activeEndpointId = newEp.id;
      }
      this._saveConfig();
      this._renderEndpointOptions();
      this._renderEndpointsEditor();
      this._onActiveEndpointChanged();
    });
    form.append(saveBtn);
  }

  // Render path mapping list and editor for the active endpoint
  _renderMappingsEditor() {
    const panel = this.elements.mappingsPanel;
    panel.innerHTML = '<h3>Path Mapping Configuration</h3>';

    const endpoint = this._getActiveEndpoint();
    if (!endpoint) {
      panel.append(
        document.createTextNode('Select or connect an endpoint first.'),
      );
      return;
    }

    panel.innerHTML += `<p class="qb-mapping-desc">Configure directory mapping rules for <strong>${endpoint.name}</strong>. Rules are matched top-to-bottom.</p>`;

    const list = document.createElement('div');
    list.className = 'qb-config-list';
    panel.append(list);

    if (!endpoint.mappings || endpoint.mappings.length === 0) {
      list.innerHTML = '<p class="qb-empty">No mapping rules configured.</p>';
    } else {
      for (const [idx, rule] of endpoint.mappings.entries()) {
        const item = document.createElement('div');
        item.className = 'qb-config-item';

        const info = document.createElement('span');
        info.innerHTML = `<code>${rule.source}</code> &rarr; <code>${rule.target}</code>`;
        item.append(info);

        const actions = document.createElement('div');
        item.append(actions);

        // Move Up
        if (idx > 0) {
          const upBtn = document.createElement('button');
          upBtn.textContent = '▲';
          upBtn.className = 'qb-btn qb-btn-secondary';
          upBtn.addEventListener('click', () => {
            const temp = endpoint.mappings[idx];
            endpoint.mappings[idx] = endpoint.mappings[idx - 1];
            endpoint.mappings[idx - 1] = temp;
            this._saveConfig();
            this._renderMappingsEditor();
            this._renderQBPane();
          });
          actions.append(upBtn);
        }

        // Move Down
        if (idx < endpoint.mappings.length - 1) {
          const dnBtn = document.createElement('button');
          dnBtn.textContent = '▼';
          dnBtn.className = 'qb-btn qb-btn-secondary';
          dnBtn.addEventListener('click', () => {
            const temp = endpoint.mappings[idx];
            endpoint.mappings[idx] = endpoint.mappings[idx + 1];
            endpoint.mappings[idx + 1] = temp;
            this._saveConfig();
            this._renderMappingsEditor();
            this._renderQBPane();
          });
          actions.append(dnBtn);
        }

        const delBtn = document.createElement('button');
        delBtn.textContent = 'Delete';
        delBtn.className = 'qb-btn qb-btn-danger';
        delBtn.addEventListener('click', () => {
          endpoint.mappings.splice(idx, 1);
          this._saveConfig();
          this._renderMappingsEditor();
          this._renderQBPane();
        });
        actions.append(delBtn);

        list.append(item);
      }
    }

    // Add mapping rule form
    const form = document.createElement('div');
    form.className = 'qb-add-form';
    panel.append(form);

    form.innerHTML = `
      <h4>Add Mapping Rule</h4>
      <div class="qb-form-row">
        <label>qBittorrent Prefix:</label>
        <input type="text" placeholder="/downloads/complete/" id="qb-new-src">
      </div>
      <div class="qb-form-row">
        <label>Transmission Prefix:</label>
        <input type="text" placeholder="/share/Downloads/" id="qb-new-tgt">
      </div>
    `;

    const addBtn = document.createElement('button');
    addBtn.textContent = 'Add Rule';
    addBtn.className = 'qb-btn qb-btn-primary';
    addBtn.addEventListener('click', () => {
      const src = document.querySelector('#qb-new-src').value.trim();
      const tgt = document.querySelector('#qb-new-tgt').value.trim();

      if (!src || !tgt) {
        alert('All fields are required.');
        return;
      }

      if (!endpoint.mappings) {
        endpoint.mappings = [];
      }

      endpoint.mappings.push({ source: src, target: tgt });
      this._saveConfig();
      this._renderMappingsEditor();
      this._renderQBPane();
    });
    form.append(addBtn);
  }

  // Update status message / connection indicator badge
  _updateConnectionIndicator() {
    const ind = this.elements.indicator;
    ind.className = 'qb-indicator';

    switch (this.connectionStatus) {
      case 'connected': {
        ind.classList.add('status-connected');
        ind.textContent = 'Connected';

        break;
      }
      case 'connecting': {
        ind.classList.add('status-connecting');
        ind.textContent = 'Connecting...';

        break;
      }
      case 'error': {
        ind.classList.add('status-error');
        ind.textContent = `Error: ${this.connectionError}`;

        break;
      }
      default: {
        ind.classList.add('status-disconnected');
        ind.textContent = 'Disconnected';
      }
    }
  }

  // Render qBittorrent torrents list (Right Pane)
  _renderQBPane() {
    if (!this.elements || !this.elements.qbPane) {
      return;
    }
    const pane = this.elements.qbPane;

    if (this.connectionStatus !== 'connected') {
      if (this.elements) {
        this.elements.qbInitialized = false;
      }
      pane.innerHTML = '<h3>qBittorrent Remote Client</h3>';
      const empty = document.createElement('div');
      empty.className = 'qb-pane-empty';
      empty.innerHTML = `
        <p>Not connected to qBittorrent.</p>
        ${this.connectionStatus === 'error' ? `<p class="qb-error-msg">Error Details: ${this.connectionError}</p>` : ''}
        <button class="qb-btn qb-btn-primary" id="qb-reconnect-btn">Connect Client</button>
      `;
      pane.append(empty);
      pane
        .querySelector('#qb-reconnect-btn')
        .addEventListener('click', () => this.connectQBittorrent());
      return;
    }

    // Sort torrents: transferable first, then untransferable, then transferred. Within groups, sort alphabetically.
    const sorted = this.qbTorrents.toSorted((a, b) => {
      const stateA = this._getTorrentState(a).status;
      const stateB = this._getTorrentState(b).status;

      const priority = {
        transferable: 0,
        transferred: 2,
        untransferable: 1,
      };

      const prioA = priority[stateA] ?? 99;
      const prioB = priority[stateB] ?? 99;

      if (prioA !== prioB) {
        return prioA - prioB;
      }

      return a.name.localeCompare(b.name);
    });

    if (this.elements.qbInitialized && this.elements.qbTbody) {
      // Already initialized, just refresh the list & count without rebuilding controls
      const listContainer = this.elements.qbTbody;
      const { scrollTop } = listContainer;

      listContainer.innerHTML = '';
      this.elements.qbCountSpan.textContent = `Total: ${this.qbTorrents.length} torrents`;

      if (this.qbTorrents.length === 0) {
        const emptyDiv = document.createElement('div');
        emptyDiv.className = 'qb-empty';
        emptyDiv.textContent = 'No torrents found in qBittorrent.';
        listContainer.append(emptyDiv);
      } else {
        for (const qbTor of sorted) {
          const card = this._createQBRow(qbTor);
          listContainer.append(card);
        }
      }

      // Re-apply current filtering
      this._filterQBRows();

      // Restore scroll position
      listContainer.scrollTop = scrollTop;
      return;
    }

    // First time initializing
    pane.innerHTML = '<h3>qBittorrent Remote Client</h3>';

    // Controls bar
    const ctrlBar = document.createElement('div');
    ctrlBar.className = 'qb-search-bar';
    pane.append(ctrlBar);

    const searchInput = document.createElement('input');
    searchInput.type = 'text';
    searchInput.placeholder = 'Filter qB...';
    searchInput.addEventListener('input', () => this._filterQBRows());
    ctrlBar.append(searchInput);
    this.elements.qbSearchInput = searchInput;

    const filterSelect = document.createElement('select');
    filterSelect.innerHTML = `
      <option value="all">All Torrents</option>
      <option value="untransferred" selected>Not Transferred</option>
      <option value="transferred">Transferred</option>
      <option value="transferable">Transferable Only</option>
    `;
    filterSelect.addEventListener('change', () => this._filterQBRows());
    ctrlBar.append(filterSelect);
    this.elements.qbFilterSelect = filterSelect;

    const batchBtn = document.createElement('button');
    batchBtn.textContent = 'Import Checked';
    batchBtn.className = 'qb-btn qb-btn-primary';
    batchBtn.addEventListener('click', () => this.migrateAllTransferable());
    ctrlBar.append(batchBtn);

    // Count info
    const countSpan = document.createElement('div');
    countSpan.className = 'qb-pane-count';
    countSpan.textContent = `Total: ${this.qbTorrents.length} torrents`;
    pane.append(countSpan);
    this.elements.qbCountSpan = countSpan;

    const container = document.createElement('div');
    container.className = 'qb-torrents-container';
    pane.append(container);

    const listContainer = document.createElement('div');
    listContainer.className = 'qb-torrents-list';
    container.append(listContainer);
    this.elements.qbTbody = listContainer;

    if (this.qbTorrents.length === 0) {
      const emptyDiv = document.createElement('div');
      emptyDiv.className = 'qb-empty';
      emptyDiv.textContent = 'No torrents found in qBittorrent.';
      listContainer.append(emptyDiv);
    } else {
      for (const qbTor of sorted) {
        const card = this._createQBRow(qbTor);
        listContainer.append(card);
      }
    }

    this.elements.qbInitialized = true;

    // Apply initial filters
    this._filterQBRows();
  }

  // Create a row/card for qB torrent table/list
  _createQBRow(qbTor) {
    const hash = qbTor.hash.toLowerCase();
    const card = document.createElement('div');
    card.id = `qb-row-${hash}`;
    card.className = 'qb-torrent-card';
    card.dataset.name = qbTor.name.toLowerCase();
    card.dataset.hash = hash;
    card.dataset.tags = (qbTor.tags || '').toLowerCase();

    // Cache the row node references to update inline later
    this._updateQBRowData(card, qbTor);

    return card;
  }

  _updateQBRow(hash) {
    if (!this.elements || !this.elements.root) {
      return;
    }
    const card = this.elements.root.querySelector(`#qb-row-${hash}`);
    if (!card) {
      return;
    }
    const qbTor = this.qbTorrents.find((t) => t.hash.toLowerCase() === hash);
    if (!qbTor) {
      return;
    }
    this._updateQBRowData(card, qbTor);
  }

  _updateQBRowData(card, qbTor) {
    card.innerHTML = '';
    const hash = qbTor.hash.toLowerCase();
    const state = this._getTorrentState(qbTor);

    // Set datasets to filter easily
    card.dataset.status = state.status; // 'transferred', 'transferable', 'untransferable'

    // 1. Title Row
    const titleRow = document.createElement('div');
    titleRow.className = 'qb-card-title-row';

    const titleSpan = document.createElement('div');
    titleSpan.className = 'qb-card-title';
    titleSpan.textContent = qbTor.name;
    titleSpan.title = `Hash: ${hash}\nState in qB: ${qbTor.state}`;
    titleRow.append(titleSpan);

    if (qbTor.tags) {
      const tagsDiv = document.createElement('div');
      tagsDiv.className = 'qb-card-tags';
      for (const t of qbTor.tags.split(',')) {
        const tagSpan = document.createElement('span');
        tagSpan.className = 'qb-tag-badge';
        tagSpan.textContent = t.trim();
        tagsDiv.append(tagSpan);
      }
      titleRow.append(tagsDiv);
    }
    card.append(titleRow);

    // 2. Meta Row (Size & Status Badge)
    const metaRow = document.createElement('div');
    metaRow.className = 'qb-card-meta-row';

    const sizeSpan = document.createElement('span');
    sizeSpan.className = 'qb-card-size';
    sizeSpan.textContent = Formatter.size(qbTor.size);
    metaRow.append(sizeSpan);

    const badge = document.createElement('span');
    badge.className = `qb-badge badge-${state.status}`;
    if (state.status === 'transferred') {
      badge.textContent = 'Transferred';
    } else if (state.status === 'transferable') {
      badge.textContent = 'Transferable';
    } else {
      badge.textContent =
        state.reason === 'No Path Map' ? 'Unmapped' : 'Untransferable';
    }
    metaRow.append(badge);
    card.append(metaRow);

    // 3. Paths Row
    const pathsDiv = document.createElement('div');
    pathsDiv.className = 'qb-card-paths';

    const srcDiv = document.createElement('div');
    srcDiv.className = 'qb-card-path-src';
    srcDiv.innerHTML = `qB: <code>${qbTor.save_path}</code>`;
    pathsDiv.append(srcDiv);

    const resolved =
      this.manualOverrides[hash] || this._resolvePath(qbTor.save_path);
    const destDiv = document.createElement('div');
    destDiv.className = 'qb-card-path-dest';
    destDiv.innerHTML = resolved
      ? `Map: <code>${resolved}</code>`
      : `Map: <span class="qb-text-warn">No rule match</span>`;
    pathsDiv.append(destDiv);
    card.append(pathsDiv);

    // 4. Log Row
    const taskLog = this.transferStates[hash];
    if (taskLog) {
      const logDiv = document.createElement('div');
      logDiv.className = 'qb-card-log';
      logDiv.textContent = taskLog;
      card.append(logDiv);
    } else if (state.status !== 'transferred' && state.reason !== 'Ready') {
      const reasonDiv = document.createElement('div');
      reasonDiv.className = 'qb-card-log';
      reasonDiv.style.backgroundColor = 'var(--color-bg-warn)';
      reasonDiv.style.color = 'var(--color-fg-warn)';
      reasonDiv.textContent = state.reason;
      card.append(reasonDiv);
    }

    // 5. Actions Row
    const actionsRow = document.createElement('div');
    actionsRow.className = 'qb-card-actions';

    // Manual override button
    const overrideBtn = document.createElement('button');
    overrideBtn.className = 'qb-btn qb-btn-secondary';
    overrideBtn.textContent = '✏️ Path';
    overrideBtn.title = 'Temporarily override destination directory';
    overrideBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      const current =
        this.manualOverrides[hash] ||
        this._resolvePath(qbTor.save_path) ||
        qbTor.save_path;
      const customPath = prompt(
        `Enter custom Transmission path for:\n"${qbTor.name}"`,
        current,
      );
      if (customPath !== null) {
        const path = customPath.trim();
        const endpoint = this._getActiveEndpoint();
        if (path) {
          // 1. Save override
          this.manualOverrides[hash] = path;

          // 2. Save prefix mapping rule globally
          if (endpoint) {
            const mapRule = endpoint.mappings.find(
              (m) => m.source === qbTor.save_path,
            );
            if (mapRule) {
              mapRule.target = path;
            } else {
              endpoint.mappings.push({
                source: qbTor.save_path,
                target: path,
              });
            }
            this._saveConfig();
            this._renderMappingsEditor();
          }
        } else {
          // Clear both override and mapping
          delete this.manualOverrides[hash];
          if (endpoint) {
            endpoint.mappings = endpoint.mappings.filter(
              (m) => m.source !== qbTor.save_path,
            );
            this._saveConfig();
            this._renderMappingsEditor();
          }
        }
        this._renderQBPane();
      }
    });
    actionsRow.append(overrideBtn);

    // Main import button
    const importBtn = document.createElement('button');
    importBtn.className = 'qb-btn qb-btn-primary';

    if (state.status === 'transferred') {
      importBtn.textContent = 'Verify Again';
      importBtn.className = 'qb-btn qb-btn-secondary';
      importBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        if (state.trTor) {
          this.controller._verifyTorrents([state.trTor]);
          this.refreshTransmission();
        }
      });
    } else {
      importBtn.textContent = 'Import & Verify';
      setEnabled(importBtn, state.status === 'transferable');
      importBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.migrateTorrent(qbTor);
      });
    }
    actionsRow.append(importBtn);

    card.append(actionsRow);
  }

  // Filter local qB list
  _filterQBRows() {
    const tbody = this.elements.qbTbody;
    if (!tbody) {
      return;
    }

    const query = this.elements.qbSearchInput.value.toLowerCase();
    const filter = this.elements.qbFilterSelect.value; // 'all', 'transferred', 'untransferred', 'transferable'

    for (const row of tbody.children) {
      if (row.classList.contains('qb-empty')) {
        continue;
      }

      const name = row.dataset.name || '';
      const hash = row.dataset.hash || '';
      const tags = row.dataset.tags || '';
      const status = row.dataset.status || '';

      const matchesSearch =
        name.includes(query) || hash.includes(query) || tags.includes(query);
      let matchesFilter = true;

      switch (filter) {
        case 'transferred': {
          matchesFilter = status === 'transferred';

          break;
        }
        case 'untransferred': {
          matchesFilter = status !== 'transferred';

          break;
        }
        case 'transferable': {
          matchesFilter = status === 'transferable';

          break;
        }
        // No default
      }

      row.classList.toggle('hidden', !(matchesSearch && matchesFilter));
    }
  }
}
