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
