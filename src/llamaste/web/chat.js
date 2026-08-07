/* Llamaste Chat — SSE streaming, tool visualization, conversation management */

(function () {
  'use strict';

  // --- State ---
  var conversationId = generateId();
  var streaming = false;
  var currentAssistantEl = null;
  var currentTextBuffer = '';
  var abortController = null;

  // --- DOM refs ---
  var messagesEl = document.getElementById('messages');
  var inputEl = document.getElementById('message-input');
  var sendBtn = document.getElementById('send-btn');
  var connStatus = document.getElementById('connection-status');

  // --- Init ---
  inputEl.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  });

  // Auto-resize textarea
  inputEl.addEventListener('input', function () {
    this.style.height = 'auto';
    this.style.height = Math.min(this.scrollHeight, 120) + 'px';
  });

  loadConversations();

  // --- Public API (called from HTML) ---
  window.sendMessage = sendMessage;
  window.newConversation = newConversation;
  window.loadConversation = loadConversation;

  // --- Send Message ---
  function sendMessage() {
    var text = inputEl.value.trim();
    if (!text || streaming) return;

    appendMessage('user', text);
    inputEl.value = '';
    inputEl.style.height = 'auto';
    setStreaming(true);

    // Create assistant message placeholder
    currentAssistantEl = appendMessage('assistant', '');
    currentTextBuffer = '';
    showTypingIndicator(currentAssistantEl);

    abortController = new AbortController();

    fetch('/llamaste/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify({
        message: text,
        conversation_id: conversationId,
        stream: true
      }),
      signal: abortController.signal
    })
    .then(function (response) {
      if (!response.ok) {
        throw new Error('HTTP ' + response.status);
      }
      setConnected(true);
      readSSEStream(response.body.getReader());
    })
    .catch(function (err) {
      if (err.name === 'AbortError') return;
      removeTypingIndicator(currentAssistantEl);
      setMessageContent(currentAssistantEl, 'Connection error: ' + err.message);
      setConnected(false);
      setStreaming(false);
    });
  }

  // --- SSE Stream Reader ---
  function readSSEStream(reader) {
    var decoder = new TextDecoder();
    var buffer = '';

    function pump() {
      reader.read().then(function (result) {
        if (result.done) {
          finishStream();
          return;
        }

        buffer += decoder.decode(result.value, { stream: true });
        var lines = buffer.split('\n');
        buffer = lines.pop(); // keep incomplete line in buffer

        for (var i = 0; i < lines.length; i++) {
          processSSELine(lines[i]);
        }

        pump();
      }).catch(function (err) {
        if (err.name !== 'AbortError') {
          finishStream();
        }
      });
    }

    pump();
  }

  function processSSELine(line) {
    line = line.trim();
    if (!line || line.startsWith(':')) return; // empty or comment

    if (!line.startsWith('data: ')) return;
    var payload = line.substring(6);

    if (payload === '[DONE]') {
      finishStream();
      return;
    }

    var data;
    try {
      data = JSON.parse(payload);
    } catch (e) {
      return; // skip malformed JSON
    }

    removeTypingIndicator(currentAssistantEl);

    switch (data.type) {
      case 'token':
        currentTextBuffer += data.content;
        setMessageContent(currentAssistantEl, renderMarkdown(currentTextBuffer));
        scrollToBottom();
        break;

      case 'tool_call':
        appendToolCall(currentAssistantEl, data.name, data.arguments, null);
        scrollToBottom();
        break;

      case 'tool_result':
        updateToolResult(currentAssistantEl, data.name, data.result);
        scrollToBottom();
        break;

      case 'done':
        if (data.content && !currentTextBuffer) {
          currentTextBuffer = data.content;
          setMessageContent(currentAssistantEl, renderMarkdown(currentTextBuffer));
        }
        finishStream();
        break;

      case 'error':
        currentTextBuffer += '\n\n**Error:** ' + (data.content || 'Unknown error');
        setMessageContent(currentAssistantEl, renderMarkdown(currentTextBuffer));
        finishStream();
        break;
    }
  }

  function finishStream() {
    if (!streaming) return;
    removeTypingIndicator(currentAssistantEl);

    // If we never got any content, show a fallback
    if (currentAssistantEl && !currentTextBuffer && !currentAssistantEl.querySelector('.tool-call')) {
      setMessageContent(currentAssistantEl, '<span style="color:var(--text-muted)">No response received.</span>');
    }

    currentAssistantEl = null;
    currentTextBuffer = '';
    setStreaming(false);
    scrollToBottom();
  }

  // --- Message DOM Helpers ---
  function appendMessage(role, content) {
    var el = document.createElement('div');
    el.className = 'message ' + role;
    if (content) {
      el.innerHTML = role === 'user' ? escapeHtml(content) : renderMarkdown(content);
    }
    messagesEl.appendChild(el);
    scrollToBottom();
    return el;
  }

  function setMessageContent(el, html) {
    if (!el) return;
    // Preserve tool call cards
    var tools = el.querySelectorAll('.tool-call');
    el.innerHTML = html;
    for (var i = 0; i < tools.length; i++) {
      el.appendChild(tools[i]);
    }
  }

  function showTypingIndicator(el) {
    if (!el || el.querySelector('.typing-indicator')) return;
    var indicator = document.createElement('div');
    indicator.className = 'typing-indicator';
    indicator.innerHTML = '<span></span><span></span><span></span>';
    el.appendChild(indicator);
  }

  function removeTypingIndicator(el) {
    if (!el) return;
    var indicator = el.querySelector('.typing-indicator');
    if (indicator) indicator.remove();
  }

  // --- Tool Call Visualization ---
  function appendToolCall(parentEl, name, args, result) {
    var card = document.createElement('div');
    card.className = 'tool-call';
    card.setAttribute('data-tool-name', name);

    var header = document.createElement('div');
    header.className = 'tool-call-header';
    header.innerHTML =
      '<span class="tool-icon">&#9654;</span>' +
      '<span class="tool-name">' + escapeHtml(name) + '</span>' +
      '<span class="tool-status">running...</span>';

    header.addEventListener('click', function () {
      var body = card.querySelector('.tool-call-body');
      var icon = header.querySelector('.tool-icon');
      if (body.classList.contains('visible')) {
        body.classList.remove('visible');
        icon.classList.remove('expanded');
      } else {
        body.classList.add('visible');
        icon.classList.add('expanded');
      }
    });

    var body = document.createElement('div');
    body.className = 'tool-call-body';

    if (args) {
      var argsFormatted = typeof args === 'string' ? args : JSON.stringify(args, null, 2);
      body.innerHTML =
        '<div class="tool-label">Arguments</div>' +
        '<pre>' + escapeHtml(argsFormatted) + '</pre>';
    }

    if (result) {
      var resultFormatted = typeof result === 'string' ? result : JSON.stringify(result, null, 2);
      body.innerHTML +=
        '<div class="tool-label">Result</div>' +
        '<pre>' + escapeHtml(resultFormatted) + '</pre>';
      header.querySelector('.tool-status').textContent = 'done';
    }

    card.appendChild(header);
    card.appendChild(body);
    parentEl.appendChild(card);
  }

  function updateToolResult(parentEl, name, result) {
    if (!parentEl) return;
    var cards = parentEl.querySelectorAll('.tool-call');
    // Find the last card matching this tool name that has no result yet
    for (var i = cards.length - 1; i >= 0; i--) {
      var card = cards[i];
      if (card.getAttribute('data-tool-name') === name) {
        var body = card.querySelector('.tool-call-body');
        var status = card.querySelector('.tool-status');
        var resultFormatted = typeof result === 'string' ? result : JSON.stringify(result, null, 2);
        body.innerHTML +=
          '<div class="tool-label">Result</div>' +
          '<pre>' + escapeHtml(resultFormatted) + '</pre>';
        if (status) status.textContent = 'done';
        return;
      }
    }
    // If no matching card found, create one
    appendToolCall(parentEl, name, null, result);
  }

  // --- Markdown Rendering (basic) ---
  function renderMarkdown(text) {
    if (!text) return '';

    // Escape HTML first
    var html = escapeHtml(text);

    // Code blocks (``` ... ```)
    html = html.replace(/```(\w*)\n([\s\S]*?)```/g, function (m, lang, code) {
      return '<pre><code>' + code.trim() + '</code></pre>';
    });

    // Inline code (`...`)
    html = html.replace(/`([^`\n]+)`/g, '<code>$1</code>');

    // Bold (**...**)
    html = html.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');

    // Italic (*...*)
    html = html.replace(/\*([^*]+)\*/g, '<em>$1</em>');

    // Line breaks
    html = html.replace(/\n\n/g, '</p><p>');
    html = html.replace(/\n/g, '<br>');

    // Wrap in paragraph if needed
    if (!html.startsWith('<pre>') && !html.startsWith('<p>')) {
      html = '<p>' + html + '</p>';
    }

    return html;
  }

  // --- Conversation Management ---
  function newConversation() {
    conversationId = generateId();
    messagesEl.innerHTML = '<div class="message system">Welcome to Llamaste. The LLM is your operating system. Ask anything.</div>';
    highlightActiveConversation(conversationId);
  }

  function loadConversations() {
    fetch('/llamaste/conversations', { credentials: 'include' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        setConnected(true);
        renderConversationList(data.conversations || []);
      })
      .catch(function () {
        // Server not available yet, that is fine
        setConnected(false);
      });
  }

  function renderConversationList(conversations) {
    var stripEl = document.getElementById('conv-strip');
    if (!stripEl) return;

    // Remove all existing chips (but keep #new-conv-btn)
    var existing = stripEl.querySelectorAll('.conv-chip');
    for (var i = 0; i < existing.length; i++) {
      existing[i].remove();
    }

    // Insert chips before the new-conv button
    var newBtn = document.getElementById('new-conv-btn');

    for (var i = 0; i < conversations.length; i++) {
      var conv = conversations[i];
      var chip = document.createElement('button');
      chip.className = 'conv-chip';
      if (conv.id === conversationId) chip.className += ' active';
      chip.textContent = conv.title || 'Conv ' + conv.id.substring(0, 8);
      chip.setAttribute('data-conv-id', conv.id);
      chip.addEventListener('click', (function (id, title) {
        return function () { loadConversation(id, title); };
      })(conv.id, conv.title));
      stripEl.insertBefore(chip, newBtn);
    }
  }

  function loadConversation(id, title) {
    conversationId = id;
    highlightActiveConversation(id);

    // Future: fetch conversation history from server
    messagesEl.innerHTML = '<div class="message system">Loaded conversation. Previous messages not yet implemented.</div>';
  }

  function highlightActiveConversation(id) {
    var chips = document.querySelectorAll('.conv-chip');
    for (var i = 0; i < chips.length; i++) {
      if (chips[i].getAttribute('data-conv-id') === id) {
        chips[i].classList.add('active');
      } else {
        chips[i].classList.remove('active');
      }
    }
  }

  // --- Utility ---
  function setStreaming(value) {
    streaming = value;
    sendBtn.disabled = value;
    inputEl.disabled = value;
    if (!value) inputEl.focus();
  }

  function setConnected(online) {
    connStatus.className = online ? 'online' : 'offline';
  }

  function scrollToBottom() {
    requestAnimationFrame(function () {
      messagesEl.scrollTop = messagesEl.scrollHeight;
    });
  }

  function escapeHtml(text) {
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(text));
    return div.innerHTML;
  }

  function generateId() {
    var chars = 'abcdefghijklmnopqrstuvwxyz0123456789';
    var id = '';
    for (var i = 0; i < 16; i++) {
      id += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return id;
  }

})();
