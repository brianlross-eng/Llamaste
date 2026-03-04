/* Llamaste Notifications — Toast display layer */

(function () {
  'use strict';

  var container = document.getElementById('toast-container');
  var badge = document.getElementById('notif-badge');
  var AUTO_DISMISS_MS = 8000;

  // Border colors by toast type
  var TYPE_COLORS = {
    info:    'var(--accent)',
    alert:   'var(--warning)',
    success: 'var(--success)',
    error:   'var(--danger)'
  };

  /**
   * showToast(title, body, type)
   * Display a toast notification that auto-removes after 8 seconds.
   *   title  — bold heading text
   *   body   — description text
   *   type   — 'info' | 'alert' | 'success' | 'error' (default: 'info')
   */
  function showToast(title, body, type) {
    if (!container) return;
    type = type || 'info';
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

    // Auto-dismiss after timeout
    var timer = setTimeout(function () {
      removeToast(toast);
    }, AUTO_DISMISS_MS);

    // Store timer so close button can cancel it
    toast._dismissTimer = timer;
  }

  /**
   * removeToast(el)
   * Animate out and remove a toast element.
   */
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

  /**
   * updateBadge(count)
   * Show or hide the notification badge in the status bar.
   *   count — number of unread notifications. 0 hides the badge.
   */
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

  // Export to window
  window.showToast = showToast;
  window.updateBadge = updateBadge;

  // --- SSE connection to notification stream ---
  var notifSource = new EventSource('/llamaste/notifications');
  var unreadCount = 0;

  notifSource.onmessage = function (event) {
    try {
      var data = JSON.parse(event.data);
      showToast(data.title, data.body, data.type);
      unreadCount++;
      updateBadge(unreadCount);
    } catch (e) {
      // ignore parse errors (heartbeat comments, etc.)
    }
  };

  notifSource.onerror = function () {
    // Auto-reconnect is built into EventSource
  };

  // Reset badge when user views chat
  var chatTab = document.querySelector('.tab[data-tab="chat"]');
  if (chatTab) {
    chatTab.addEventListener('click', function () {
      unreadCount = 0;
      updateBadge(0);
    });
  }

})();
