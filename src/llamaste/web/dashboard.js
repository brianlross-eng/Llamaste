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
  var dashIp = document.getElementById('dash-ip');
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
        dashIp.textContent = '--';
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
    dashIp.textContent = data.ip || '--';
    dashMode.textContent = data.mode || '--';
    dashUptime.textContent = data.uptime ? formatUptime(data.uptime) : '--';

    // --- Dashboard tab: Model card ---
    dashModel.textContent = data.model || 'No model loaded';
    dashSpeed.textContent = tokPerSec + ' tok/s';

    // Show/hide download button based on model status
    var dlBtn = document.getElementById('dash-download-btn');
    if (dlBtn) {
      var m = (data.model || '').toLowerCase();
      var hasModel = m && m !== 'none' && m !== '--' && m.indexOf('stub') === -1 && m.indexOf('no model') === -1;
      dlBtn.style.display = hasModel ? 'none' : '';
    }

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

  // --- Model download button ---
  function initDownloadButton() {
    var dlBtn = document.getElementById('dash-download-btn');
    if (!dlBtn) return;

    dlBtn.addEventListener('click', function () {
      dlBtn.disabled = true;
      dlBtn.textContent = 'Checking recommended model...';

      // First check what model is recommended
      fetch('/llamaste/model/recommended', { credentials: 'include' })
        .then(function (r) { return r.json(); })
        .then(function (rec) {
          if (!rec.recommended) {
            dlBtn.textContent = 'Not enough RAM for any model';
            setTimeout(function () {
              dlBtn.textContent = 'Download Recommended Model';
              dlBtn.disabled = false;
            }, 5000);
            return;
          }

          if (rec.already_downloaded) {
            dlBtn.textContent = rec.model_name + ' already downloaded';
            setTimeout(function () {
              dlBtn.textContent = 'Download Recommended Model';
              dlBtn.disabled = false;
            }, 5000);
            return;
          }

          var sizeMb = rec.approx_download_mb || 0;
          var sizeStr = sizeMb > 1024
            ? (sizeMb / 1024).toFixed(1) + ' GB'
            : sizeMb + ' MB';
          dlBtn.textContent = 'Downloading ' + rec.model_name + ' (' + sizeStr + ')...';

          // Start the download
          return fetch('/llamaste/model/download-recommended', { method: 'POST', credentials: 'include' })
            .then(function (r) { return r.json(); })
            .then(function (result) {
              if (result.status === 'success' || result.status === 'already_exists') {
                dlBtn.textContent = 'Downloaded! Restart to load.';
                dlBtn.className = 'btn btn-success';
              } else {
                dlBtn.textContent = 'Download failed: ' + (result.error || 'unknown');
                dlBtn.className = 'btn btn-danger';
                setTimeout(function () {
                  dlBtn.textContent = 'Retry Download';
                  dlBtn.className = 'btn btn-primary';
                  dlBtn.disabled = false;
                }, 5000);
              }
            });
        })
        .catch(function (err) {
          dlBtn.textContent = 'Error: ' + err.message;
          setTimeout(function () {
            dlBtn.textContent = 'Download Recommended Model';
            dlBtn.className = 'btn btn-primary';
            dlBtn.disabled = false;
          }, 5000);
        });
    });
  }

  initDownloadButton();

})();
