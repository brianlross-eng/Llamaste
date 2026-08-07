/* Llamaste Dashboard — Status bar updater + Dashboard tab + System tab */

(function () {
  'use strict';

  var POLL_INTERVAL = 5000; // 5 seconds
  var pollTimer = null;

  // --- DOM refs: Status bar ---
  var statusClock = document.getElementById('status-clock');
  var statusModel = document.getElementById('status-model');
  var statusSpeed = document.getElementById('status-speed');
  var statusRam = document.getElementById('status-ram');
  var statusIp = document.getElementById('status-ip');

  // --- DOM refs: Dashboard tab ---
  var dashCpu = document.getElementById('dash-cpu');
  var dashCpuBar = document.getElementById('dash-cpu-bar');
  var dashTemp = document.getElementById('dash-temp');
  var dashRam = document.getElementById('dash-ram');
  var dashRamBar = document.getElementById('dash-ram-bar');
  var dashDisk = document.getElementById('dash-disk');
  var dashDiskBar = document.getElementById('dash-disk-bar');
  var dashCpuModel = document.getElementById('dash-cpu-model');
  var dashCpuCores = document.getElementById('dash-cpu-cores');
  var dashGpu = document.getElementById('dash-gpu');
  var dashAvx2 = document.getElementById('dash-avx2');
  var dashMode = document.getElementById('dash-mode');
  var dashUptime = document.getElementById('dash-uptime');
  var dashModel = document.getElementById('dash-model');
  var dashSpeed = document.getElementById('dash-speed');

  // --- DOM refs: System tab ---
  var sysModel = document.getElementById('sys-model');
  var sysIp = document.getElementById('sys-ip');
  var sysMode = document.getElementById('sys-mode');

  // --- Clock (updates every second) ---
  function updateClock() {
    var now = new Date();
    var hours = now.getHours();
    var minutes = now.getMinutes();
    var ampm = hours >= 12 ? 'PM' : 'AM';
    hours = hours % 12;
    if (hours === 0) hours = 12;
    var minuteStr = minutes < 10 ? '0' + minutes : '' + minutes;

    var months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    var month = months[now.getMonth()];
    var day = now.getDate();

    statusClock.textContent = hours + ':' + minuteStr + ' ' + ampm + '  ' + month + ' ' + day;
  }

  updateClock();
  setInterval(updateClock, 1000);

  // --- Data polling ---
  fetchDashboard();
  pollTimer = setInterval(fetchDashboard, POLL_INTERVAL);

  // Pause polling when browser tab hidden, resume when visible
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      clearInterval(pollTimer);
      pollTimer = null;
    } else {
      fetchDashboard();
      pollTimer = setInterval(fetchDashboard, POLL_INTERVAL);
    }
  });

  // --- Fetch and update ---
  function fetchDashboard() {
    fetch('/llamaste/system', { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(updateAll)
      .catch(function () {
        // Server not available, show dashes in status bar
        statusModel.textContent = '--';
        statusSpeed.textContent = '-- tok/s';
        statusRam.textContent = '-- GB';
        statusIp.textContent = '--';

        // Dashboard tab
        dashCpu.textContent = '--%';
        dashCpuBar.style.width = '0%';
        dashTemp.textContent = '--';
        dashTemp.className = 'value';
        dashRam.textContent = '-- / -- MB';
        dashRamBar.style.width = '0%';
        dashDisk.textContent = '-- / -- GB';
        dashDiskBar.style.width = '0%';
        dashCpuModel.textContent = '--';
        dashCpuCores.textContent = '--';
        dashGpu.textContent = '--';
        dashAvx2.textContent = '--';
        var nc = document.getElementById('dash-networks');
        if (nc) nc.innerHTML = '<div class="dash-row"><span class="label">IP</span><span class="value">--</span></div>';
        dashMode.textContent = '--';
        dashUptime.textContent = '--';
        dashModel.textContent = '--';
        dashSpeed.textContent = '-- tok/s';

        // System tab
        sysModel.textContent = '--';
        sysIp.textContent = '--';
        sysMode.textContent = '--';
      });
  }

  function updateAll(data) {
    // --- Status bar ---
    statusModel.textContent = data.model || '--';
    statusIp.textContent = data.ip || '--';

    var tokPerSec = typeof data.tokens_per_sec === 'number' ? data.tokens_per_sec.toFixed(1) : '--';
    statusSpeed.textContent = tokPerSec + ' tok/s';

    var ramTotalMb = data.ram_total_mb || 0;
    var ramUsedMb = data.ram_used_mb || 0;
    var ramTotalGb = (ramTotalMb / 1024).toFixed(1);
    var ramUsedGb = (ramUsedMb / 1024).toFixed(1);
    statusRam.textContent = ramUsedGb + '/' + ramTotalGb + ' GB';

    // --- Dashboard tab: CPU card ---
    var cpuPct = typeof data.cpu_percent === 'number' ? data.cpu_percent : 0;
    dashCpu.textContent = cpuPct.toFixed(0) + '%';
    dashCpuBar.style.width = cpuPct + '%';
    setBarColor(dashCpuBar, cpuPct);

    // Temperature
    if (typeof data.temperature === 'number') {
      dashTemp.textContent = data.temperature.toFixed(0) + '\u00B0C';
      if (data.temperature < 60) {
        dashTemp.className = 'value temp-green';
      } else if (data.temperature < 80) {
        dashTemp.className = 'value temp-yellow';
      } else {
        dashTemp.className = 'value temp-red';
      }
    } else {
      dashTemp.textContent = '--';
      dashTemp.className = 'value';
    }

    // --- Dashboard tab: Memory card ---
    var ramPct = ramTotalMb > 0 ? (ramUsedMb / ramTotalMb) * 100 : 0;
    dashRam.textContent = ramUsedMb.toFixed(0) + ' / ' + ramTotalMb.toFixed(0) + ' MB';
    dashRamBar.style.width = ramPct.toFixed(1) + '%';
    setBarColor(dashRamBar, ramPct);

    // --- Dashboard tab: Storage card ---
    var diskUsed = data.disk_used_gb || 0;
    var diskTotal = data.disk_total_gb || 1;
    var diskPct = (diskUsed / diskTotal) * 100;
    dashDisk.textContent = diskUsed.toFixed(1) + ' / ' + diskTotal.toFixed(1) + ' GB';
    dashDiskBar.style.width = diskPct.toFixed(1) + '%';
    setBarColor(dashDiskBar, diskPct);

    // --- Dashboard tab: Hardware card ---
    dashCpuModel.textContent = data.cpu_model || '--';
    dashCpuCores.textContent = typeof data.cpu_cores === 'number' ? data.cpu_cores : '--';
    dashGpu.textContent = data.gpu_detected ? (data.gpu_name || 'Detected') : 'None';
    dashAvx2.textContent = data.has_avx2 ? 'Yes' : (data.has_avx2 === false ? 'No' : '--');

    // --- Dashboard tab: Network card ---
    var netContainer = document.getElementById('dash-networks');
    if (netContainer) {
      var nets = data.networks || [];
      if (nets.length > 0) {
        var html = '';
        for (var i = 0; i < nets.length; i++) {
          var n = nets[i];
          var icon = n.type === 'wifi' ? '📶' : '🔌';
          var label = n.name + ' (' + n.type + ')';
          html += '<div class="dash-row">' +
            '<span class="label">' + icon + ' ' + label + '</span>' +
            '<span class="value">' + (n.ip || '--') + '</span>' +
            '</div>';
        }
        netContainer.innerHTML = html;
      } else {
        netContainer.innerHTML = '<div class="dash-row">' +
          '<span class="label">IP</span>' +
          '<span class="value">' + (data.ip || '--') + '</span></div>';
      }
    }
    dashMode.textContent = data.mode || '--';
    dashUptime.textContent = data.uptime ? formatUptime(data.uptime) : '--';

    // --- Dashboard tab: Model card ---
    // Show the logical model name: collapse a sharded filename
    // (…-00001-of-00002.gguf) to its base so it matches the picker (#12).
    var modelName = data.model || '';
    modelName = modelName.replace(/-\d{5}-of-\d{5}(\.gguf)?$/i, '');
    dashModel.textContent = modelName || 'No model loaded';
    dashSpeed.textContent = tokPerSec + ' tok/s';

    // Download button always visible — user may want to download additional models

    // --- System tab ---
    sysModel.textContent = data.model || '--';
    sysIp.textContent = data.ip || '--';
    sysMode.textContent = data.mode || '--';
  }

  // --- Helpers ---
  function setBarColor(barEl, pct) {
    barEl.classList.remove('warn', 'danger');
    if (pct >= 90) {
      barEl.classList.add('danger');
    } else if (pct >= 70) {
      barEl.classList.add('warn');
    }
  }

  function formatUptime(seconds) {
    var d = Math.floor(seconds / 86400);
    var h = Math.floor((seconds % 86400) / 3600);
    var m = Math.floor((seconds % 3600) / 60);

    if (d > 0) return d + 'd ' + h + 'h';
    if (h > 0) return h + 'h ' + m + 'm';
    return m + 'm';
  }

  // --- Model picker ---
  function initModelPicker() {
    var dlBtn = document.getElementById('dash-download-btn');
    var selBtn = document.getElementById('model-select-btn');
    var selEl = document.getElementById('model-select');
    var statusEl = document.getElementById('model-status');
    if (!dlBtn || !selEl) return;

    var tierEl = document.getElementById('model-download-tier');
    // Track state
    var recommended = null;
    var downloadedModels = [];

    function setStatus(msg) { if (statusEl) statusEl.textContent = msg; }

    // Load available model tiers into download dropdown
    function refreshTierList() {
      fetch('/llamaste/model/tiers', { credentials: 'include' })
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (!tierEl) return;
          tierEl.innerHTML = '';
          var tiers = data.tiers || [];
          tiers.forEach(function(t) {
            var opt = document.createElement('option');
            opt.value = JSON.stringify({ repo_id: t.repo_id, filename: t.filename, name: t.name });
            var sizeStr = t.approx_size_mb > 1024
              ? (t.approx_size_mb / 1024).toFixed(1) + ' GB'
              : t.approx_size_mb + ' MB';
            var label = t.name + ' (' + sizeStr + ')';
            if (t.downloaded) label += ' [downloaded]';
            else if (t.recommended) label += ' [recommended]';
            else if (!t.fits_ram) label += ' [needs more RAM]';
            opt.textContent = label;
            if (t.recommended) opt.selected = true;
            tierEl.appendChild(opt);
          });
        }).catch(function() {});
    }

    // Load downloaded models into select dropdown
    function refreshModelList() {
      Promise.all([
        fetch('/llamaste/model/list', { credentials: 'include' }).then(function(r) { return r.json(); }),
        fetch('/llamaste/model/recommended', { credentials: 'include' }).then(function(r) { return r.json(); })
      ]).then(function(results) {
        var listData = results[0];
        recommended = results[1];
        downloadedModels = listData.models || [];

        selEl.innerHTML = '';

        if (downloadedModels.length === 0) {
          var opt = document.createElement('option');
          opt.value = '';
          opt.textContent = 'No models downloaded';
          selEl.appendChild(opt);
          selBtn.disabled = true;
        } else {
          var autoOpt = document.createElement('option');
          autoOpt.value = '';
          autoOpt.textContent = '(Auto-select by RAM)';
          selEl.appendChild(autoOpt);

          downloadedModels.forEach(function(m) {
            var opt = document.createElement('option');
            opt.value = '/data/models/' + m.filename;   // shard-1 for sharded models
            var label = (m.display_name || m.filename);
            if (m.size_human) label += ' (' + m.size_human + ')';
            if (m.sharded && m.complete === false) {
              label += ' [incomplete: ' + m.shards_present + '/' + m.shard_count + ' parts]';
              opt.disabled = true;   // can't load a model that's missing shards
            }
            opt.textContent = label;
            selEl.appendChild(opt);
          });
          selBtn.disabled = false;
        }
      }).catch(function() {
        setStatus('Failed to load model list');
      });
    }

    // Format bytes to human-readable
    function fmtBytes(b) {
      if (b >= 1073741824) return (b / 1073741824).toFixed(1) + ' GB';
      if (b >= 1048576) return (b / 1048576).toFixed(0) + ' MB';
      return (b / 1024).toFixed(0) + ' KB';
    }

    // Poll download progress
    var dlPollTimer = null;
    function pollDownloadProgress() {
      fetch('/llamaste/model/download/progress', { credentials: 'include' })
        .then(function(r) { return r.json(); })
        .then(function(p) {
          if (p.finished) {
            clearInterval(dlPollTimer);
            dlPollTimer = null;
            var r = p.result || {};
            if (r.status === 'success' || r.status === 'already_exists') {
              dlBtn.textContent = 'Downloaded!';
              dlBtn.disabled = false;
              setStatus('Model downloaded. Reboot to load.');
              refreshModelList();
              refreshTierList();
            } else {
              dlBtn.textContent = 'Download Failed';
              setStatus('Error: ' + (p.error || r.error || 'unknown'));
              setTimeout(function() { dlBtn.disabled = false; dlBtn.textContent = 'Retry'; }, 5000);
            }
            return;
          }
          if (!p.active) {
            clearInterval(dlPollTimer);
            dlPollTimer = null;
            dlBtn.disabled = false;
            dlBtn.textContent = 'Download';
            return;
          }
          // Show progress
          var shardText = p.total_shards > 1
            ? ' (shard ' + p.current_shard + '/' + p.total_shards + ')'
            : '';
          var pct = p.current_file_total > 0
            ? Math.round(p.current_file_bytes / p.current_file_total * 100)
            : 0;
          dlBtn.textContent = 'Downloading ' + pct + '%' + shardText;
          setStatus('Downloaded ' + fmtBytes(p.total_downloaded_bytes) + shardText);
        })
        .catch(function() {});
    }

    // Download selected model tier (async)
    dlBtn.addEventListener('click', function() {
      var tierVal = tierEl ? tierEl.value : '';
      if (!tierVal) return;

      var tier;
      try { tier = JSON.parse(tierVal); } catch(e) { return; }

      dlBtn.disabled = true;
      dlBtn.textContent = 'Starting download...';
      setStatus('Requesting download of ' + (tier.name || 'model') + '...');

      fetch('/llamaste/model/download-tier', {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(tier)
      })
        .then(function(r) { return r.json(); })
        .then(function(result) {
          if (result.status === 'started' || result.status === 'busy') {
            if (!dlPollTimer) dlPollTimer = setInterval(pollDownloadProgress, 2000);
            pollDownloadProgress();
          } else if (result.status === 'already_exists') {
            dlBtn.textContent = 'Already Downloaded';
            setStatus('Model already downloaded.');
            refreshModelList();
            refreshTierList();
          } else {
            dlBtn.textContent = 'Download Failed';
            setStatus('Error: ' + (result.error || 'unknown'));
            setTimeout(function() { dlBtn.disabled = false; dlBtn.textContent = 'Download'; }, 5000);
          }
        })
        .catch(function(err) {
          dlBtn.textContent = 'Error';
          setStatus(err.message);
          setTimeout(function() { dlBtn.disabled = false; dlBtn.textContent = 'Download'; }, 5000);
        });
    });

    // Check for in-progress download on page load
    fetch('/llamaste/model/download/progress', { credentials: 'include' })
      .then(function(r) { return r.json(); })
      .then(function(p) {
        if (p.active) {
          dlBtn.disabled = true;
          if (!dlPollTimer) dlPollTimer = setInterval(pollDownloadProgress, 2000);
          pollDownloadProgress();
        }
      }).catch(function() {});

    // Select model
    selBtn.addEventListener('click', function() {
      var path = selEl.value;
      selBtn.disabled = true;
      setStatus('Setting model...');

      fetch('/llamaste/model/select', {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: path })
      })
        .then(function(r) { return r.json(); })
        .then(function(result) {
          if (result.status === 'ok') {
            setStatus(result.message);
          } else {
            setStatus('Error: ' + (result.error || 'unknown'));
          }
          selBtn.disabled = false;
        })
        .catch(function(err) {
          setStatus('Error: ' + err.message);
          selBtn.disabled = false;
        });
    });

    // USB scan/import
    var usbBtn = document.getElementById('model-usb-btn');
    if (usbBtn) {
      usbBtn.addEventListener('click', function() {
        usbBtn.disabled = true;
        usbBtn.textContent = 'Scanning...';
        setStatus('Scanning USB drives for GGUF files...');

        fetch('/llamaste/model/usb/scan', { credentials: 'include' })
          .then(function(r) { return r.json(); })
          .then(function(result) {
            if (result.error) {
              setStatus(result.error);
              usbBtn.textContent = 'Scan USB Drive';
              usbBtn.disabled = false;
              return;
            }
            var files = result.gguf_files || [];
            if (files.length === 0) {
              setStatus('No GGUF files found on USB drives.');
              usbBtn.textContent = 'Scan USB Drive';
              usbBtn.disabled = false;
              return;
            }
            // Show found files and let user pick one to import
            setStatus('Found ' + files.length + ' file(s). Importing first...');
            var f = files[0];
            usbBtn.textContent = 'Importing ' + f.filename + '...';

            fetch('/llamaste/model/usb/import', {
              method: 'POST',
              credentials: 'include',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ device: f.device, filename: f.filename })
            })
              .then(function(r) { return r.json(); })
              .then(function(imp) {
                if (imp.error) {
                  setStatus('Import error: ' + imp.error);
                } else {
                  setStatus('Imported ' + (imp.filename || f.filename) + '!');
                  refreshModelList();
                }
                usbBtn.textContent = 'Scan USB Drive';
                usbBtn.disabled = false;
              })
              .catch(function(err) {
                setStatus('Import error: ' + err.message);
                usbBtn.textContent = 'Scan USB Drive';
                usbBtn.disabled = false;
              });
          })
          .catch(function(err) {
            setStatus('Scan error: ' + err.message);
            usbBtn.textContent = 'Scan USB Drive';
            usbBtn.disabled = false;
          });
      });
    }

    refreshModelList();
    refreshTierList();
  }

  initModelPicker();

})();
