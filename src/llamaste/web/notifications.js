/* Llamaste Notifications — Toast display + history panel */

(function () {
  'use strict';

  var container = document.getElementById('toast-container');
  var badge = document.getElementById('notif-badge');
  var panel = document.getElementById('notif-panel');
  var panelList = document.getElementById('notif-panel-list');
  var clearBtn = document.getElementById('notif-clear-btn');
  var AUTO_DISMISS_MS = 8000;
  var MAX_HISTORY = 50;

  // Notification history array
  var history = [];
  var unreadCount = 0;
  var panelOpen = false;

  // Border colors by toast type
  var TYPE_COLORS = {
    info:    'var(--accent)',
    alert:   'var(--warning)',
    success: 'var(--success)',
    error:   'var(--danger)'
  };

  var TYPE_ICONS = {
    info:    '\u2139',
    alert:   '\u26A0',
    success: '\u2714',
    error:   '\u2718'
  };

  /**
   * showToast(title, body, type)
   * Display a toast notification and add to history.
   */
  function showToast(title, body, type) {
    type = type || 'info';

    // Add to history
    history.unshift({
      title: title || '',
      body: body || '',
      type: type,
      time: new Date()
    });
    if (history.length > MAX_HISTORY) history.pop();

    // Update panel if open
    if (panelOpen) renderPanel();

    if (!container) return;
    var borderColor = TYPE_COLORS[type] || TYPE_COLORS.info;

    var toast = document.createElement('div');
    toast.className = 'toast';
    toast.style.borderLeftColor = borderColor;

    // Title row with close button
    var titleRow = document.createElement('div');
    titleRow.style.display = 'flex';
    titleRow.style.justifyContent = 'space-between';
    titleRow.style.alignItems = 'flex-start';

    var titleEl = document.createElement('div');
    titleEl.className = 'toast-title';
    titleEl.textContent = title || '';

    var closeBtn = document.createElement('span');
    closeBtn.textContent = '\u00D7';
    closeBtn.style.cursor = 'pointer';
    closeBtn.style.color = 'var(--text-muted)';
    closeBtn.style.fontSize = '16px';
    closeBtn.style.lineHeight = '1';
    closeBtn.style.marginLeft = '12px';
    closeBtn.style.flexShrink = '0';
    closeBtn.onclick = function () {
      removeToast(toast);
    };

    titleRow.appendChild(titleEl);
    titleRow.appendChild(closeBtn);
    toast.appendChild(titleRow);

    if (body) {
      var bodyEl = document.createElement('div');
      bodyEl.className = 'toast-body';
      bodyEl.textContent = body;
      toast.appendChild(bodyEl);
    }

    container.appendChild(toast);

    var timer = setTimeout(function () {
      removeToast(toast);
    }, AUTO_DISMISS_MS);

    toast._dismissTimer = timer;
  }

  function removeToast(el) {
    if (!el || !el.parentNode) return;
    if (el._dismissTimer) clearTimeout(el._dismissTimer);
    el.style.transition = 'opacity 0.3s ease, transform 0.3s ease';
    el.style.opacity = '0';
    el.style.transform = 'translateX(100%)';
    setTimeout(function () {
      if (el.parentNode) el.parentNode.removeChild(el);
    }, 300);
  }

  function updateBadge(count) {
    if (!badge) return;
    if (count > 0) {
      badge.textContent = count > 99 ? '99+' : '' + count;
      badge.classList.remove('hidden');
    } else {
      badge.textContent = '0';
      badge.classList.add('hidden');
    }
  }

  function fmtTime(d) {
    var h = d.getHours(), m = d.getMinutes();
    var ampm = h >= 12 ? 'PM' : 'AM';
    h = h % 12 || 12;
    return h + ':' + (m < 10 ? '0' : '') + m + ' ' + ampm;
  }

  function renderPanel() {
    if (!panelList) return;
    if (history.length === 0) {
      panelList.innerHTML = '<div class="notif-empty">No notifications</div>';
      return;
    }
    var html = '';
    for (var i = 0; i < history.length; i++) {
      var n = history[i];
      var icon = TYPE_ICONS[n.type] || TYPE_ICONS.info;
      var color = '';
      if (n.type === 'error') color = 'color:var(--danger)';
      else if (n.type === 'success') color = 'color:var(--success)';
      else if (n.type === 'alert') color = 'color:var(--warning)';
      else color = 'color:var(--accent)';
      html += '<div class="notif-item">';
      html += '<span class="notif-icon" style="' + color + '">' + icon + '</span>';
      html += '<div class="notif-content">';
      html += '<div class="notif-title">' + escHtml(n.title) + '</div>';
      if (n.body) html += '<div class="notif-body">' + escHtml(n.body) + '</div>';
      html += '</div>';
      html += '<span class="notif-time">' + fmtTime(n.time) + '</span>';
      html += '</div>';
    }
    panelList.innerHTML = html;
  }

  function escHtml(s) {
    var d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
  }

  function togglePanel() {
    panelOpen = !panelOpen;
    if (panel) {
      panel.style.display = panelOpen ? 'flex' : 'none';
      if (panelOpen) {
        renderPanel();
        // Mark all as read
        unreadCount = 0;
        updateBadge(0);
      }
    }
  }

  // Close panel when clicking outside
  document.addEventListener('click', function (e) {
    if (!panelOpen) return;
    if (panel && !panel.contains(e.target) && e.target !== badge) {
      panelOpen = false;
      panel.style.display = 'none';
    }
  });

  // Badge click toggles panel
  if (badge) {
    badge.style.cursor = 'pointer';
    badge.addEventListener('click', function (e) {
      e.stopPropagation();
      togglePanel();
    });
  }

  // Clear all button
  if (clearBtn) {
    clearBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      history = [];
      unreadCount = 0;
      updateBadge(0);
      renderPanel();
    });
  }

  // Export to window
  window.showToast = showToast;
  window.updateBadge = updateBadge;

  // --- SSE connection to notification stream ---
  var notifSource = new EventSource('/llamaste/notifications');

  notifSource.onmessage = function (event) {
    try {
      var data = JSON.parse(event.data);
      showToast(data.title, data.body, data.type);
      if (!panelOpen) {
        unreadCount++;
        updateBadge(unreadCount);
      }
    } catch (e) {
      // ignore parse errors (heartbeat comments, etc.)
    }
  };

  notifSource.onerror = function () {
    // Auto-reconnect is built into EventSource
  };

})();
