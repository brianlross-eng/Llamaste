/* Llamaste Files — File browser with directory listing and file preview */

(function () {
  'use strict';

  // --- State ---
  var currentPath = '/data/';

  // --- DOM refs ---
  var pathEl = document.getElementById('files-path');
  var listEl = document.getElementById('files-list');
  var previewEl = document.getElementById('files-preview');

  // --- Public API (called from HTML) ---
  window.filesRefresh = filesRefresh;
  window.filesUpload = filesUpload;
  window.filesNewFolder = filesNewFolder;

  // --- Refresh / list directory ---
  function filesRefresh() {
    if (!listEl) return;
    listEl.innerHTML = '<div class="files-loading">Loading...</div>';
    if (pathEl) pathEl.textContent = currentPath;

    fetch('/llamaste/files?path=' + encodeURIComponent(currentPath), { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        if (data.error) {
          showError(data.error);
          return;
        }
        renderFileList(data);
      })
      .catch(function (err) {
        showError('Failed to load directory: ' + err.message);
      });
  }

  // --- Render file list ---
  function renderFileList(data) {
    listEl.innerHTML = '';
    var entries = data.entries || [];

    // Parent directory entry (if not at /data/)
    if (currentPath !== '/data/' && currentPath !== '/data') {
      var parentRow = document.createElement('div');
      parentRow.className = 'file-row file-dir';
      parentRow.innerHTML =
        '<span class="file-icon">&#128193;</span>' +
        '<span class="file-name">..</span>' +
        '<span class="file-size"></span>' +
        '<span class="file-date"></span>';
      parentRow.addEventListener('click', function () {
        navigateUp();
      });
      listEl.appendChild(parentRow);
    }

    if (entries.length === 0) {
      var empty = document.createElement('div');
      empty.className = 'files-empty';
      empty.textContent = 'Directory is empty';
      listEl.appendChild(empty);
      return;
    }

    // Sort: directories first, then files alphabetically
    entries.sort(function (a, b) {
      if (a.type === 'directory' && b.type !== 'directory') return -1;
      if (a.type !== 'directory' && b.type === 'directory') return 1;
      return a.name.localeCompare(b.name);
    });

    for (var i = 0; i < entries.length; i++) {
      var entry = entries[i];
      var row = createFileRow(entry);
      listEl.appendChild(row);
    }
  }

  // --- Create a file row element ---
  function createFileRow(entry) {
    var row = document.createElement('div');
    var isDir = entry.type === 'directory';
    row.className = 'file-row ' + (isDir ? 'file-dir' : 'file-file');

    var icon = isDir ? '&#128193;' : '&#128196;';
    var size = isDir ? '--' : formatSize(entry.size || 0);
    var date = entry.modified ? formatDate(entry.modified) : '--';

    row.innerHTML =
      '<span class="file-icon">' + icon + '</span>' +
      '<span class="file-name">' + escapeHtml(entry.name) + '</span>' +
      '<span class="file-size">' + size + '</span>' +
      '<span class="file-date">' + date + '</span>';

    if (isDir) {
      row.addEventListener('click', (function (name) {
        return function () { navigateInto(name); };
      })(entry.name));
    } else {
      row.addEventListener('click', (function (name) {
        return function () { previewFile(name); };
      })(entry.name));
    }

    return row;
  }

  // --- Navigation ---
  function navigateInto(dirName) {
    var path = currentPath;
    if (path.charAt(path.length - 1) !== '/') path += '/';
    currentPath = path + dirName + '/';
    clearPreview();
    filesRefresh();
  }

  function navigateUp() {
    // Remove trailing slash, then go up one level
    var path = currentPath;
    if (path.charAt(path.length - 1) === '/') path = path.substring(0, path.length - 1);
    var lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
      currentPath = path.substring(0, lastSlash + 1);
    } else {
      currentPath = '/data/';
    }
    // Never go above /data/
    if (currentPath !== '/data/' && currentPath !== '/data' && currentPath.indexOf('/data/') !== 0) {
      currentPath = '/data/';
    }
    clearPreview();
    filesRefresh();
  }

  // --- File preview ---
  function previewFile(fileName) {
    if (!previewEl) return;
    var filePath = currentPath;
    if (filePath.charAt(filePath.length - 1) !== '/') filePath += '/';
    filePath += fileName;

    previewEl.innerHTML = '<div class="preview-loading">Loading preview...</div>';

    // Highlight selected row
    var rows = listEl.querySelectorAll('.file-row');
    for (var i = 0; i < rows.length; i++) {
      rows[i].classList.remove('selected');
    }
    // Find the row with this filename and mark it selected
    for (var j = 0; j < rows.length; j++) {
      var nameEl = rows[j].querySelector('.file-name');
      if (nameEl && nameEl.textContent === fileName) {
        rows[j].classList.add('selected');
        break;
      }
    }

    fetch('/llamaste/files?path=' + encodeURIComponent(filePath) + '&action=read', { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        if (data.error) {
          previewEl.innerHTML = '<div class="preview-error">' + escapeHtml(data.error) + '</div>';
          return;
        }
        renderPreview(fileName, data);
      })
      .catch(function (err) {
        previewEl.innerHTML = '<div class="preview-error">Failed to read file: ' + escapeHtml(err.message) + '</div>';
      });
  }

  function renderPreview(fileName, data) {
    var header = '<div class="preview-header">' +
      '<span class="preview-filename">' + escapeHtml(fileName) + '</span>' +
      '<span class="preview-size">' + formatSize(data.size || 0) + '</span>' +
      (data.truncated ? '<span class="preview-truncated">(truncated)</span>' : '') +
      '</div>';

    var content = '<pre class="preview-content">' + escapeHtml(data.content || '') + '</pre>';

    previewEl.innerHTML = header + content;
  }

  function clearPreview() {
    if (previewEl) {
      previewEl.innerHTML = '<div class="preview-placeholder">Select a file to preview</div>';
    }
  }

  // --- Stub actions ---
  function filesUpload() {
    llamasteAlert('File upload coming soon.');
  }

  function filesNewFolder() {
    llamasteAlert('New folder coming soon.');
  }

  // --- Error display ---
  function showError(message) {
    listEl.innerHTML = '<div class="files-error">' + escapeHtml(message) + '</div>';
  }

  // --- Formatting helpers ---
  function formatSize(bytes) {
    if (bytes === 0) return '0 B';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
  }

  function formatDate(timestamp) {
    var d = new Date(timestamp * 1000);
    var months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    var month = months[d.getMonth()];
    var day = d.getDate();
    var hours = d.getHours();
    var minutes = d.getMinutes();
    var minuteStr = minutes < 10 ? '0' + minutes : '' + minutes;
    var hourStr = hours < 10 ? '0' + hours : '' + hours;
    return month + ' ' + day + ' ' + hourStr + ':' + minuteStr;
  }

  function escapeHtml(text) {
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(text));
    return div.innerHTML;
  }

})();
