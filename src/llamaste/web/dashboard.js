/* Llamaste Dashboard — System stats polling */

(function () {
  'use strict';

  var POLL_INTERVAL = 5000; // 5 seconds
  var pollTimer = null;

  // --- DOM refs ---
  var els = {
    model:    document.getElementById('dash-model'),
    uptime:   document.getElementById('dash-uptime'),
    ip:       document.getElementById('dash-ip'),
    cpu:      document.getElementById('dash-cpu'),
    cpuBar:   document.getElementById('dash-cpu-bar'),
    temp:     document.getElementById('dash-temp'),
    ram:      document.getElementById('dash-ram'),
    ramBar:   document.getElementById('dash-ram-bar'),
    disk:     document.getElementById('dash-disk'),
    diskBar:  document.getElementById('dash-disk-bar')
  };

  // --- Start polling ---
  fetchDashboard();
  pollTimer = setInterval(fetchDashboard, POLL_INTERVAL);

  // Pause polling when tab hidden, resume when visible
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
    fetch('/llamaste/system')
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(updateDashboard)
      .catch(function () {
        // Server not available, show dashes
        els.model.textContent = '--';
        els.uptime.textContent = '--';
        els.ip.textContent = '--';
        els.cpu.textContent = '--%';
        els.cpuBar.style.width = '0%';
        els.temp.textContent = '--';
        els.temp.className = 'value';
        els.ram.textContent = '-- / -- MB';
        els.ramBar.style.width = '0%';
        els.disk.textContent = '-- / -- GB';
        els.diskBar.style.width = '0%';
      });
  }

  function updateDashboard(data) {
    // Model
    els.model.textContent = data.model || '--';

    // Uptime
    els.uptime.textContent = data.uptime ? formatUptime(data.uptime) : '--';

    // IP
    els.ip.textContent = data.ip || '--';

    // CPU
    var cpuPct = typeof data.cpu_percent === 'number' ? data.cpu_percent : 0;
    els.cpu.textContent = cpuPct.toFixed(0) + '%';
    els.cpuBar.style.width = cpuPct + '%';
    setBarColor(els.cpuBar, cpuPct);

    // Temperature
    if (typeof data.temperature === 'number') {
      els.temp.textContent = data.temperature.toFixed(0) + '\u00B0C';
      if (data.temperature < 60) {
        els.temp.className = 'value temp-green';
      } else if (data.temperature < 80) {
        els.temp.className = 'value temp-yellow';
      } else {
        els.temp.className = 'value temp-red';
      }
    } else {
      els.temp.textContent = '--';
      els.temp.className = 'value';
    }

    // RAM
    var ramUsed = data.ram_used_mb || 0;
    var ramTotal = data.ram_total_mb || 1;
    var ramPct = (ramUsed / ramTotal) * 100;
    els.ram.textContent = ramUsed.toFixed(0) + ' / ' + ramTotal.toFixed(0) + ' MB';
    els.ramBar.style.width = ramPct.toFixed(1) + '%';
    setBarColor(els.ramBar, ramPct);

    // Disk
    var diskUsed = data.disk_used_gb || 0;
    var diskTotal = data.disk_total_gb || 1;
    var diskPct = (diskUsed / diskTotal) * 100;
    els.disk.textContent = diskUsed.toFixed(1) + ' / ' + diskTotal.toFixed(1) + ' GB';
    els.diskBar.style.width = diskPct.toFixed(1) + '%';
    setBarColor(els.diskBar, diskPct);
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

})();
