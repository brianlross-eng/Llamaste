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
    fetch('/llamaste/schedules')
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

  // --- Public refresh function ---
  // Called when switching to the System tab.
  // Fetches fresh data and updates the sections that dashboard.js does NOT handle.
  function systemRefresh() {
    // Fetch system info for About section
    fetch('/llamaste/system')
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        renderAbout(data);
      })
      .catch(function () {
        renderAbout(null);
      });

    // Fetch scheduled tasks separately
    fetchSchedules();
  }

  // Export to window so switchTab can call it
  window.systemRefresh = systemRefresh;

  // Initial render with empty state
  renderScheduledTasks([]);

})();
