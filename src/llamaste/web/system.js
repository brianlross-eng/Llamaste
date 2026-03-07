/* Llamaste System Tab — Scheduled tasks, about info, system actions */

(function () {
  'use strict';

  // --- DOM refs ---
  var scheduleCard = null;
  var aboutCard = null;

  // Find the schedule and about cards in the system panel
  function findCards() {
    var cards = document.querySelectorAll('#panel-system .system-card');
    for (var i = 0; i < cards.length; i++) {
      var h3 = cards[i].querySelector('h3');
      if (!h3) continue;
      var title = h3.textContent.toLowerCase();
      if (title.indexOf('scheduled') !== -1) scheduleCard = cards[i];
      if (title.indexOf('about') !== -1) aboutCard = cards[i];
    }
  }

  findCards();

  // --- Scheduled Tasks ---
  function renderScheduledTasks(tasks) {
    if (!scheduleCard) return;
    var body = scheduleCard.querySelector('.schedule-list');
    if (!body) {
      var existing = scheduleCard.querySelector('p');
      body = document.createElement('div');
      body.className = 'schedule-list';
      if (existing) {
        scheduleCard.replaceChild(body, existing);
      } else {
        scheduleCard.appendChild(body);
      }
    }

    // Clear existing content
    while (body.firstChild) body.removeChild(body.firstChild);

    if (!tasks || tasks.length === 0) {
      var empty = document.createElement('p');
      empty.className = 'text-muted';
      empty.textContent = 'No scheduled tasks.';
      body.appendChild(empty);
      return;
    }

    for (var i = 0; i < tasks.length; i++) {
      var task = tasks[i];
      var row = document.createElement('div');
      row.className = 'schedule-row';

      var statusDot = document.createElement('span');
      statusDot.className = 'schedule-dot ' + (task.enabled ? 'dot-active' : 'dot-inactive');
      row.appendChild(statusDot);

      var info = document.createElement('div');
      info.className = 'schedule-info';

      var prompt = document.createElement('div');
      prompt.className = 'schedule-prompt';
      var promptText = task.prompt || '';
      prompt.textContent = promptText.length > 60 ? promptText.substring(0, 57) + '...' : promptText;
      info.appendChild(prompt);

      var meta = document.createElement('div');
      meta.className = 'schedule-meta text-muted';
      var schedule = task.cron ? 'cron: ' + task.cron :
                     task.every_seconds ? 'every ' + task.every_seconds + 's' :
                     task.at ? 'at ' + new Date(task.at * 1000).toLocaleString() : 'one-shot';
      var nextRun = task.next_run > 0 ? ' | next: ' + new Date(task.next_run * 1000).toLocaleTimeString() : '';
      meta.textContent = schedule + nextRun;
      info.appendChild(meta);

      row.appendChild(info);
      body.appendChild(row);
    }
  }

  function fetchSchedules() {
    fetch('/llamaste/schedules', { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        renderScheduledTasks(data.tasks || []);
      })
      .catch(function () {
        renderScheduledTasks([]);
      });
  }

  // --- About section update (uses textContent to avoid XSS) ---
  function renderAbout(data) {
    if (!aboutCard) return;
    // Clear and rebuild with safe DOM methods
    while (aboutCard.firstChild) aboutCard.removeChild(aboutCard.firstChild);
    var h3 = document.createElement('h3');
    h3.textContent = 'About';
    aboutCard.appendChild(h3);
    var verP = document.createElement('p');
    verP.className = 'text-muted';
    verP.textContent = 'Llamaste LLM-OS v0.1';
    aboutCard.appendChild(verP);

    if (data && typeof data.uptime === 'number') {
      aboutCard.appendChild(makeDashRow('Uptime', formatUptime(data.uptime)));
    }

    if (data && typeof data.disk_total_gb === 'number') {
      var diskUsed = (data.disk_used_gb || 0).toFixed(1);
      var diskTotal = data.disk_total_gb.toFixed(1);
      aboutCard.appendChild(makeDashRow('Disk', diskUsed + ' / ' + diskTotal + ' GB'));
    }

    if (data && data.cpu_model) {
      aboutCard.appendChild(makeDashRow('CPU', data.cpu_model));
    }
  }

  // --- MCP card update ---
  var g_mcpUrl = 'http://llamaste.local/mcp';  // kept in sync by renderMcpCard

  function renderMcpCard(data) {
    var urlEl = document.getElementById('mcp-endpoint-url');
    var snippetEl = document.getElementById('mcp-config-snippet');
    if (!urlEl || !snippetEl) return;

    // Use the device's actual IP if available, fallback to llamaste.local
    var host = 'llamaste.local';
    if (data && data.ip && data.ip !== '0.0.0.0' && data.ip !== '--') {
      host = data.ip;
    }
    g_mcpUrl = 'http://' + host + '/mcp';
    urlEl.textContent = g_mcpUrl;

    // Snippet will be fully populated once fetchMcpKey() completes;
    // set a placeholder with just the URL for now.
    updateMcpSnippet(null);
  }

  // Build + set the config snippet.  apiKey may be null (show placeholder).
  function updateMcpSnippet(apiKey) {
    var snippetEl = document.getElementById('mcp-config-snippet');
    if (!snippetEl) return;
    var cfg = {
      mcpServers: {
        llamaste: {
          type: 'http',
          url: g_mcpUrl,
          headers: { Authorization: 'Bearer ' + (apiKey || '<api-key>') }
        }
      }
    };
    snippetEl.textContent = JSON.stringify(cfg, null, 2);
  }

  // Fetch the API key and update the key row + snippet.
  function fetchMcpKey() {
    var keyEl   = document.getElementById('mcp-api-key');
    var copyBtn = document.getElementById('mcp-copy-key');
    var regenBtn = document.getElementById('mcp-regen-key');
    if (!keyEl) return;

    fetch('/llamaste/mcp/key', { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (d) {
        var key = d.key || '';
        keyEl.textContent = key;
        updateMcpSnippet(key);
      })
      .catch(function () {
        keyEl.textContent = '(unavailable)';
      });

    // Wire up Copy button (idempotent — guard with a flag)
    if (copyBtn && !copyBtn._wired) {
      copyBtn._wired = true;
      copyBtn.addEventListener('click', function () {
        var key = (document.getElementById('mcp-api-key') || {}).textContent || '';
        if (!key || key === '—' || key === '(unavailable)') return;
        navigator.clipboard.writeText(key).then(function () {
          var orig = copyBtn.textContent;
          copyBtn.textContent = 'Copied!';
          setTimeout(function () { copyBtn.textContent = orig; }, 1500);
        }).catch(function () {
          // Fallback for HTTP (no clipboard API)
          var ta = document.createElement('textarea');
          ta.value = key;
          ta.style.position = 'fixed';
          ta.style.opacity = '0';
          document.body.appendChild(ta);
          ta.select();
          document.execCommand('copy');
          document.body.removeChild(ta);
          var orig = copyBtn.textContent;
          copyBtn.textContent = 'Copied!';
          setTimeout(function () { copyBtn.textContent = orig; }, 1500);
        });
      });
    }

    // Wire up Regen button (idempotent)
    if (regenBtn && !regenBtn._wired) {
      regenBtn._wired = true;
      regenBtn.addEventListener('click', function () {
        if (!confirm('Generate a new API key? The old key will stop working immediately.')) return;
        regenBtn.disabled = true;
        regenBtn.textContent = '…';
        fetch('/llamaste/mcp/key/regenerate', {
          method: 'POST',
          credentials: 'include'
        })
          .then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.json();
          })
          .then(function (d) {
            var key = d.key || '';
            var keyEl2 = document.getElementById('mcp-api-key');
            if (keyEl2) keyEl2.textContent = key;
            updateMcpSnippet(key);
            regenBtn.disabled = false;
            regenBtn.textContent = 'Regen';
          })
          .catch(function () {
            regenBtn.disabled = false;
            regenBtn.textContent = 'Regen';
            alert('Failed to regenerate key.');
          });
      });
    }
  }

  function makeDashRow(label, value) {
    var row = document.createElement('div');
    row.className = 'dash-row';
    var labelEl = document.createElement('span');
    labelEl.className = 'label';
    labelEl.textContent = label;
    var valueEl = document.createElement('span');
    valueEl.className = 'value';
    valueEl.textContent = value;
    row.appendChild(labelEl);
    row.appendChild(valueEl);
    return row;
  }

  // --- Uptime formatter (matches dashboard.js) ---
  function formatUptime(seconds) {
    var d = Math.floor(seconds / 86400);
    var h = Math.floor((seconds % 86400) / 3600);
    var m = Math.floor((seconds % 3600) / 60);

    if (d > 0) return d + 'd ' + h + 'h';
    if (h > 0) return h + 'h ' + m + 'm';
    return m + 'm';
  }

  // --- Update card ---
  function updateUpdateCard() {
    fetch('/llamaste/update/status', { credentials: 'include' })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data) return;
        var verEl = document.getElementById('update-version');
        var slotEl = document.getElementById('update-slot');
        var rollbackBtn = document.getElementById('update-rollback-btn');
        var progressRow = document.getElementById('update-progress-row');

        if (verEl) verEl.textContent = data.version || '—';
        if (slotEl) slotEl.textContent = data.active_slot || '—';

        // Show rollback if inactive has a version
        if (data.inactive_version && rollbackBtn) {
          rollbackBtn.style.display = '';
          rollbackBtn.textContent = 'Rollback to ' + data.inactive_version;
        }

        // Progress during install
        if (progressRow && data.update_state && data.update_state !== 'idle') {
          progressRow.style.display = '';
          var bar = document.getElementById('update-progress-bar');
          var text = document.getElementById('update-progress-text');
          if (bar) bar.style.width = (data.update_progress || 0) + '%';
          if (text) text.textContent = data.update_state + ' (' + (data.update_progress || 0) + '%)';
        } else if (progressRow) {
          progressRow.style.display = 'none';
        }
      })
      .catch(function () {});
  }

  // Wire update buttons (once)
  var checkBtn = document.getElementById('update-check-btn');
  var rollbackBtn2 = document.getElementById('update-rollback-btn');

  if (checkBtn && !checkBtn._wired) {
    checkBtn._wired = true;
    checkBtn.addEventListener('click', function () {
      checkBtn.disabled = true;
      checkBtn.textContent = 'Checking...';
      fetch('/llamaste/update/check', { method: 'POST', credentials: 'include' })
        .then(function (r) { return r.json(); })
        .then(function (data) {
          checkBtn.disabled = false;
          checkBtn.textContent = 'Check for Updates';
          if (data.available) {
            var availRow = document.getElementById('update-available-row');
            var availEl = document.getElementById('update-available');
            if (availRow) availRow.style.display = '';
            if (availEl) availEl.textContent = 'v' + data.latest_version;
          } else {
            alert(data.message || 'No updates available');
          }
        })
        .catch(function () {
          checkBtn.disabled = false;
          checkBtn.textContent = 'Check for Updates';
        });
    });
  }

  if (rollbackBtn2 && !rollbackBtn2._wired) {
    rollbackBtn2._wired = true;
    rollbackBtn2.addEventListener('click', function () {
      if (!confirm('Roll back to the previous version? The system will need to reboot.')) return;
      rollbackBtn2.disabled = true;
      fetch('/llamaste/update/rollback', { method: 'POST', credentials: 'include' })
        .then(function (r) { return r.json(); })
        .then(function (data) {
          rollbackBtn2.disabled = false;
          if (data.success) {
            alert(data.message || 'Rollback prepared. Reboot to activate.');
          } else {
            alert('Rollback failed: ' + (data.error || 'unknown error'));
          }
        })
        .catch(function () {
          rollbackBtn2.disabled = false;
          alert('Rollback request failed.');
        });
    });
  }

  // --- WiFi card ---
  var g_wifiConnectedSsid  = '';
  var g_wifiSavedSet       = {};   // ssid → true for saved networks
  var g_wifiScanNetworks   = [];   // last scan result array
  var g_wifiSelectedOpen   = false; // true when selected network has no password

  // Map dBm signal level to 0-4 bar quality
  function dbmToBars(dbm) {
    if (!dbm || dbm >= 0) return { bars: 0, label: 'Unknown',   color: '#555' };
    if (dbm >= -50)        return { bars: 4, label: 'Excellent', color: '#00e5a0' };
    if (dbm >= -60)        return { bars: 3, label: 'Good',      color: '#7ec8a0' };
    if (dbm >= -70)        return { bars: 2, label: 'Fair',      color: '#e5c800' };
    if (dbm >= -80)        return { bars: 1, label: 'Weak',      color: '#e59300' };
    return                        { bars: 0, label: 'Poor',      color: '#e54444' };
  }

  // Render signal bars as coloured Unicode block chars
  function renderSignalBars(dbm) {
    var q    = dbmToBars(dbm);
    var segs = ['▂', '▄', '▆', '█'];
    var out  = '';
    for (var i = 0; i < 4; i++) {
      var lit = i < q.bars;
      out += '<span style="color:' + (lit ? q.color : '#333') + ';font-size:1em">' + segs[i] + '</span>';
    }
    return '<span title="' + (dbm || '?') + ' dBm — ' + q.label + '">' + out + '</span>';
  }

  // Small frequency band badge
  function wifiFreqBadge(freq) {
    if (!freq) return '';
    var band = freq < 3000 ? '2.4G' : freq < 6000 ? '5G' : '6G';
    return '<span style="font-size:0.72em;background:#1a3a5a;color:#7ec8a0;border-radius:3px;padding:1px 4px;margin-left:4px">' + band + '</span>';
  }

  function renderWifiState(data) {
    var stateEl    = document.getElementById('wifi-state');
    var ssidRow    = document.getElementById('wifi-ssid-row');
    var ssidEl     = document.getElementById('wifi-ssid');
    var sigRow     = document.getElementById('wifi-signal-row');
    var sigVal     = document.getElementById('wifi-signal-val');
    var ipRow      = document.getElementById('wifi-ip-row');
    var ipEl       = document.getElementById('wifi-ip');
    var disconnBtn = document.getElementById('wifi-disconnect-btn');
    var forgetBtn  = document.getElementById('wifi-forget-btn');
    var connBtn    = document.getElementById('wifi-connect-btn');
    var scanBtn    = document.getElementById('wifi-scan-btn');
    var form       = document.getElementById('wifi-connect-form');
    if (!stateEl) return;

    if (!data || !data.available) {
      stateEl.textContent = 'No WiFi hardware';
      stateEl.style.color = '#888';
      if (scanBtn) scanBtn.style.display = 'none';
      return;
    }

    var connected = !!(data.connected || data.state === 'COMPLETED');
    g_wifiConnectedSsid = connected ? (data.ssid || '') : '';

    if (connected) {
      stateEl.textContent = 'Connected';
      stateEl.style.color = '#00e5a0';
      if (ssidRow) {
        ssidRow.style.display = '';
        ssidEl.innerHTML = (data.ssid || '') + renderSignalBars(data.signal_dbm);
      }
      if (sigRow && sigVal && data.signal_dbm) {
        var q = dbmToBars(data.signal_dbm);
        sigVal.innerHTML = renderSignalBars(data.signal_dbm) +
          ' <span style="color:' + q.color + ';font-size:0.85em">' + data.signal_dbm + ' dBm — ' + q.label + '</span>';
        sigRow.style.display = '';
      } else if (sigRow) {
        sigRow.style.display = 'none';
      }
      if (ipRow)      { ipRow.style.display = ''; ipEl.textContent = data.ip_addr || '--'; }
      if (disconnBtn)   disconnBtn.style.display = '';
      if (forgetBtn)    forgetBtn.style.display = '';
      if (connBtn)      connBtn.style.display = 'none';
      if (form)         form.style.display = 'none';
    } else if (!data.daemon_running) {
      stateEl.textContent = 'Not available';
      stateEl.style.color = '#888';
      if (ssidRow)  ssidRow.style.display  = 'none';
      if (sigRow)   sigRow.style.display   = 'none';
      if (ipRow)    ipRow.style.display    = 'none';
      if (disconnBtn) disconnBtn.style.display = 'none';
      if (forgetBtn)  forgetBtn.style.display  = 'none';
    } else {
      stateEl.textContent = data.state || 'Disconnected';
      stateEl.style.color = '#aaa';
      if (ssidRow)  ssidRow.style.display  = 'none';
      if (sigRow)   sigRow.style.display   = 'none';
      if (ipRow)    ipRow.style.display    = 'none';
      if (disconnBtn) disconnBtn.style.display = 'none';
      if (forgetBtn)  forgetBtn.style.display  = 'none';
    }

    // Re-render scan results if any (saved/connected state may have changed)
    if (g_wifiScanNetworks.length > 0) {
      renderScanResults(g_wifiScanNetworks, g_wifiSavedSet, g_wifiConnectedSsid);
    }
  }

  function fetchWifiStatus() {
    Promise.all([
      fetch('/llamaste/wifi/status', { credentials: 'include' }).then(function(r){ return r.ok ? r.json() : null; }),
      fetch('/llamaste/wifi/list',   { method: 'POST', credentials: 'include' }).then(function(r){ return r.ok ? r.json() : null; })
    ]).then(function(results) {
      var statusData = results[0];
      var listData   = results[1];
      // Build saved set from list
      g_wifiSavedSet = {};
      if (listData && listData.networks) {
        listData.networks.forEach(function(n) { g_wifiSavedSet[n.ssid] = true; });
      }
      if (statusData) renderWifiState(statusData);
    }).catch(function() {});
  }

  function renderScanResults(networks, savedSet, connectedSsid) {
    var container = document.getElementById('wifi-scan-results');
    var connBtn   = document.getElementById('wifi-connect-btn');
    var form      = document.getElementById('wifi-connect-form');
    if (!container) return;
    while (container.firstChild) container.removeChild(container.firstChild);

    if (!networks || networks.length === 0) {
      var p = document.createElement('p');
      p.className = 'text-muted';
      p.textContent = 'No networks found.';
      container.appendChild(p);
      return;
    }
    if (form)    form.style.display = '';
    if (connBtn) connBtn.style.display = '';

    var list = document.createElement('div');
    list.style.cssText = 'max-height:200px;overflow-y:auto;margin-bottom:6px;border-radius:4px';
    for (var i = 0; i < networks.length; i++) {
      var net      = networks[i];
      var isConn   = (connectedSsid && net.ssid === connectedSsid);
      var isSaved  = !!(savedSet && savedSet[net.ssid]);
      var isOpen   = (net.security === 'OPEN');

      var row = document.createElement('div');
      row.style.cssText = 'cursor:pointer;padding:6px 8px;border-bottom:1px solid #1e2d3d;border-radius:3px;' +
        (isConn ? 'background:#0d2a1f;' : 'background:transparent;');

      // Top line: lock/open icon + SSID + freq badge + connected tick
      var topLine = document.createElement('div');
      topLine.style.cssText = 'display:flex;align-items:center;gap:4px;font-size:0.9em';
      topLine.innerHTML =
        '<span title="' + (isOpen ? 'Open network' : net.security) + '" style="font-size:0.85em">' + (isOpen ? '🔓' : '🔒') + '</span>' +
        '<span style="color:' + (isConn ? '#00e5a0' : '#e0e0e0') + ';font-weight:' + (isConn ? '600' : 'normal') + '">' + net.ssid + '</span>' +
        wifiFreqBadge(net.freq_mhz) +
        (isConn  ? '<span style="color:#00e5a0;margin-left:4px;font-size:0.8em">✓ connected</span>' : '') +
        (isSaved && !isConn ? '<span style="color:#7ec8a0;margin-left:4px;font-size:0.75em;opacity:0.7">saved</span>' : '');

      // Bottom line: signal bars + dBm + security label
      var metaLine = document.createElement('div');
      metaLine.style.cssText = 'display:flex;align-items:center;gap:6px;margin-top:2px;font-size:0.75em;color:#888';
      metaLine.innerHTML =
        renderSignalBars(net.signal_dbm) +
        '<span>' + (net.signal_dbm || '?') + ' dBm</span>' +
        '<span style="opacity:0.6">' + (net.security || 'OPEN') + '</span>';

      row.appendChild(topLine);
      row.appendChild(metaLine);

      (function (ssid, open) {
        row.addEventListener('click', function () {
          // Highlight selected row
          var rows = list.querySelectorAll('[data-wifi-row]');
          for (var j = 0; j < rows.length; j++) rows[j].style.background = 'transparent';
          row.style.background = '#0d2840';

          var inp       = document.getElementById('wifi-ssid-input');
          var pskRow    = document.getElementById('wifi-psk-row');
          var openNote  = document.getElementById('wifi-open-notice');
          var pskInp    = document.getElementById('wifi-psk-input');
          if (inp) { inp.value = ssid; }
          g_wifiSelectedOpen = open;
          if (pskRow)   pskRow.style.display   = open ? 'none' : '';
          if (openNote) openNote.style.display  = open ? ''     : 'none';
          if (pskInp && !open) { pskInp.value = ''; pskInp.focus(); }
        });
      })(net.ssid, isOpen);

      row.setAttribute('data-wifi-row', '1');
      list.appendChild(row);
    }
    container.appendChild(list);
  }

  var wifiScanBtn   = document.getElementById('wifi-scan-btn');
  var wifiConnBtn   = document.getElementById('wifi-connect-btn');
  var wifiDiscBtn   = document.getElementById('wifi-disconnect-btn');
  var wifiForgetBtn = document.getElementById('wifi-forget-btn');
  var wifiPskToggle = document.getElementById('wifi-psk-toggle');
  var wifiMsg       = document.getElementById('wifi-status-msg');

  function setWifiMsg(txt, isErr) {
    if (!wifiMsg) return;
    wifiMsg.textContent = txt;
    wifiMsg.style.color = isErr ? '#f44' : '#888';
  }

  if (wifiScanBtn && !wifiScanBtn._wired) {
    wifiScanBtn._wired = true;
    wifiScanBtn.addEventListener('click', function () {
      wifiScanBtn.disabled = true;
      wifiScanBtn.textContent = 'Scanning…';
      setWifiMsg('Scanning for networks…', false);
      fetch('/llamaste/wifi/scan', { method: 'POST', credentials: 'include' })
        .then(function (r) { return r.json(); })
        .then(function (d) {
          wifiScanBtn.disabled = false;
          wifiScanBtn.textContent = 'Scan';
          if (!d.success) {
            setWifiMsg(d.error || 'Scan failed', true);
          } else {
            setWifiMsg(d.count + ' network(s) found', false);
            g_wifiScanNetworks = d.networks || [];
            renderScanResults(g_wifiScanNetworks, g_wifiSavedSet, g_wifiConnectedSsid);
          }
        })
        .catch(function () {
          wifiScanBtn.disabled = false;
          wifiScanBtn.textContent = 'Scan';
          setWifiMsg('Scan request failed', true);
        });
    });
  }

  if (wifiConnBtn && !wifiConnBtn._wired) {
    wifiConnBtn._wired = true;
    wifiConnBtn.addEventListener('click', function () {
      var ssid = (document.getElementById('wifi-ssid-input') || {}).value || '';
      var psk  = g_wifiSelectedOpen ? '' : ((document.getElementById('wifi-psk-input') || {}).value || '');
      if (!ssid) { setWifiMsg('Select or enter a network', true); return; }
      wifiConnBtn.disabled = true;
      wifiConnBtn.textContent = 'Connecting…';
      setWifiMsg('Connecting to ' + ssid + '…', false);
      fetch('/llamaste/wifi/connect', {
        method: 'POST', credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid: ssid, psk: psk })
      })
        .then(function (r) { return r.json(); })
        .then(function (d) {
          wifiConnBtn.disabled = false;
          wifiConnBtn.textContent = 'Connect';
          if (!d.success) {
            setWifiMsg(d.error || 'Connection failed', true);
          } else {
            setWifiMsg('Connected! IP: ' + (d.ip_addr || '…'), false);
            g_wifiScanNetworks = [];
            var sc = document.getElementById('wifi-scan-results');
            if (sc) while (sc.firstChild) sc.removeChild(sc.firstChild);
            fetchWifiStatus();
          }
        })
        .catch(function () {
          wifiConnBtn.disabled = false;
          wifiConnBtn.textContent = 'Connect';
          setWifiMsg('Request failed', true);
        });
    });
  }

  if (wifiDiscBtn && !wifiDiscBtn._wired) {
    wifiDiscBtn._wired = true;
    wifiDiscBtn.addEventListener('click', function () {
      wifiDiscBtn.disabled = true;
      fetch('/llamaste/wifi/disconnect', { method: 'POST', credentials: 'include' })
        .then(function (r) { return r.json(); })
        .then(function (d) {
          wifiDiscBtn.disabled = false;
          setWifiMsg(d.success ? 'Disconnected' : (d.error || 'Failed'), !d.success);
          fetchWifiStatus();
        })
        .catch(function () { wifiDiscBtn.disabled = false; });
    });
  }

  if (wifiForgetBtn && !wifiForgetBtn._wired) {
    wifiForgetBtn._wired = true;
    wifiForgetBtn.addEventListener('click', function () {
      var ssid = g_wifiConnectedSsid;
      if (!ssid) { setWifiMsg('Not connected to a network', true); return; }
      wifiForgetBtn.disabled = true;
      setWifiMsg('Forgetting ' + ssid + '…', false);
      fetch('/llamaste/wifi/forget', {
        method: 'POST', credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid: ssid })
      })
        .then(function (r) { return r.json(); })
        .then(function (d) {
          wifiForgetBtn.disabled = false;
          setWifiMsg(d.success ? ('Forgot ' + ssid) : (d.error || 'Failed'), !d.success);
          if (d.success) fetchWifiStatus();
        })
        .catch(function () { wifiForgetBtn.disabled = false; });
    });
  }

  if (wifiPskToggle && !wifiPskToggle._wired) {
    wifiPskToggle._wired = true;
    wifiPskToggle.addEventListener('click', function () {
      var inp = document.getElementById('wifi-psk-input');
      if (!inp) return;
      inp.type = (inp.type === 'password') ? 'text' : 'password';
      wifiPskToggle.textContent = (inp.type === 'password') ? '👁' : '🙈';
    });
  }

  // Poll WiFi status every 15 seconds
  setInterval(fetchWifiStatus, 15000);
  fetchWifiStatus();

  // --- Cluster status ---
  function updateClusterCard() {
    fetch('/llamaste/cluster/status', { credentials: 'include' })
      .then(function (r) { return r.json(); })
      .then(function (data) {
        var roleEl = document.getElementById('cluster-role');
        var peersEl = document.getElementById('cluster-peers');
        var ramRow = document.getElementById('cluster-ram-row');
        var ramEl = document.getElementById('cluster-total-ram');
        var coordRow = document.getElementById('cluster-coordinator-row');
        var coordEl = document.getElementById('cluster-coordinator');

        if (!roleEl) return;

        roleEl.textContent = data.role || 'standalone';
        peersEl.textContent = String(data.peer_count || 0);

        if (data.total_ram_mb) {
          ramRow.style.display = '';
          ramEl.textContent = (data.total_ram_mb / 1024).toFixed(1) + ' GB';
        } else {
          ramRow.style.display = 'none';
        }

        if (data.coordinator && data.coordinator.hostname) {
          coordRow.style.display = '';
          coordEl.textContent = data.coordinator.hostname;
        } else {
          coordRow.style.display = 'none';
        }

        // Role badge color
        roleEl.className = 'value';
        if (data.role === 'coordinator') roleEl.style.color = '#4CAF50';
        else if (data.role === 'worker') roleEl.style.color = '#2196F3';
        else roleEl.style.color = '';
      })
      .catch(function () {});

    // Also fetch capacity info (auto-offload)
    fetch('/llamaste/cluster/capacity', { credentials: 'include' })
      .then(function (r) { return r.json(); })
      .then(function (cap) {
        var usableRow = document.getElementById('cluster-usable-row');
        var usableEl = document.getElementById('cluster-usable-ram');
        var modelRow = document.getElementById('cluster-model-row');
        var modelEl = document.getElementById('cluster-recommended-model');
        var splitRow = document.getElementById('cluster-split-row');
        var splitEl = document.getElementById('cluster-tensor-split');
        var upgradeNotice = document.getElementById('cluster-upgrade-notice');

        if (!usableRow) return;

        // Show usable RAM when we have peers
        if (cap.node_count > 1) {
          usableRow.style.display = '';
          usableEl.textContent = (cap.usable_ram_mb / 1024).toFixed(1) + ' GB';
        } else {
          usableRow.style.display = 'none';
        }

        // Show recommended model
        if (cap.recommended_model) {
          modelRow.style.display = '';
          modelEl.textContent = cap.recommended_model;
        } else {
          modelRow.style.display = 'none';
        }

        // Show tensor split when multi-node
        if (cap.tensor_split && cap.node_count > 1) {
          splitRow.style.display = '';
          splitEl.textContent = cap.tensor_split;
        } else {
          splitRow.style.display = 'none';
        }

        // Show upgrade notice
        if (cap.upgrade_available) {
          upgradeNotice.style.display = '';
        } else {
          upgradeNotice.style.display = 'none';
        }
      })
      .catch(function () {});
  }

  // Poll cluster status every 10 seconds
  setInterval(updateClusterCard, 10000);
  updateClusterCard();

  // --- Public refresh function ---
  // Called when switching to the System tab.
  // Fetches fresh data and updates the sections that dashboard.js does NOT handle.
  function systemRefresh() {
    // Fetch system info for About section + MCP URL
    fetch('/llamaste/system', { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        renderAbout(data);
        renderMcpCard(data);
        fetchMcpKey();   // populate key row + wire buttons after URL is set
      })
      .catch(function () {
        renderAbout(null);
        renderMcpCard(null);
        fetchMcpKey();
      });

    // Fetch scheduled tasks separately
    fetchSchedules();

    // Update cluster card
    updateClusterCard();

    // Update update card
    updateUpdateCard();

    // Update WiFi card
    fetchWifiStatus();
  }

  // Export to window so switchTab can call it
  window.systemRefresh = systemRefresh;

  // --- Power controls ---
  var shutdownBtn = document.getElementById('btn-shutdown');
  var rebootBtn = document.getElementById('btn-reboot');

  function powerAction(action) {
    var label = action === 'shutdown' ? 'shut down' : 'reboot';
    if (!confirm('Are you sure you want to ' + label + '?')) return;

    fetch('/llamaste/' + action, { method: 'POST', credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function () {
        document.body.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100vh;color:#aaa;font-size:1.2em">' +
          (action === 'reboot' ? 'Rebooting...' : 'Shutting down...') + '</div>';
      })
      .catch(function (e) {
        alert('Failed: ' + e.message);
      });
  }

  if (shutdownBtn) shutdownBtn.addEventListener('click', function () { powerAction('shutdown'); });
  if (rebootBtn) rebootBtn.addEventListener('click', function () { powerAction('reboot'); });

  // Initial render with empty state
  renderScheduledTasks([]);

  // --- Network Configuration ---
  var netModeSelect = document.getElementById('net-mode-select');
  var netStaticFields = document.getElementById('net-static-fields');
  var netSaveBtn = document.getElementById('net-save-btn');
  var netStatus = document.getElementById('net-status');

  if (netModeSelect) {
    netModeSelect.addEventListener('change', function () {
      netStaticFields.style.display = netModeSelect.value === 'static' ? '' : 'none';
    });
  }

  // Load current network config
  function loadNetConfig() {
    fetch('/llamaste/network/config', { credentials: 'include' })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data || !data.config) return;
        var cfg = data.config;
        if (cfg.mode === 'static') {
          netModeSelect.value = 'static';
          netStaticFields.style.display = '';
          if (cfg.ip) document.getElementById('net-ip').value = cfg.ip;
          if (cfg.netmask) document.getElementById('net-netmask').value = cfg.netmask;
          if (cfg.gateway) document.getElementById('net-gateway').value = cfg.gateway;
          if (cfg.dns) document.getElementById('net-dns').value = cfg.dns;
        }
        // Show active IP info
        if (data.active_ip) {
          document.getElementById('sys-ip').textContent = data.active_ip;
        }
      })
      .catch(function () {});
  }

  if (netModeSelect) loadNetConfig();

  if (netSaveBtn) {
    netSaveBtn.addEventListener('click', function () {
      var mode = netModeSelect.value;
      var config = { mode: mode };

      if (mode === 'static') {
        var ip = document.getElementById('net-ip').value.trim();
        if (!ip) {
          netStatus.textContent = 'IP address is required';
          netStatus.style.color = '#ff6b6b';
          return;
        }
        config.ip = ip;
        config.netmask = document.getElementById('net-netmask').value.trim() || '255.255.255.0';
        config.gateway = document.getElementById('net-gateway').value.trim();
        config.dns = document.getElementById('net-dns').value.trim() || '8.8.8.8';
      }

      // Save via network config API
      fetch('/llamaste/network/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        credentials: 'include',
        body: JSON.stringify(config)
      })
        .then(function (r) {
          if (!r.ok) throw new Error('HTTP ' + r.status);
          return r.json();
        })
        .then(function () {
          netStatus.textContent = mode === 'dhcp'
            ? 'Saved. DHCP will be used on next reboot.'
            : 'Saved. Static IP will be applied on next reboot.';
          netStatus.style.color = '#00e5a0';
        })
        .catch(function (err) {
          netStatus.textContent = 'Save failed: ' + err.message;
          netStatus.style.color = '#ff6b6b';
        });
    });
  }

})();
