// Painel admin do cb-server. Sem framework, sem dependências externas.
// Regra de segurança: todo dado vindo do servidor (nomes de jogador, resources, linhas de log, saída do console) é
// tratado como não confiável e só entra na página por textContent / nós de texto (nunca como HTML).
'use strict';

(function () {
  const $ = (id) => document.getElementById(id);

  // Cria um elemento; filhos que não são nós viram texto.
  function el(tag, props, ...children) {
    const e = document.createElement(tag);
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (k === 'class') e.className = v;
        else if (k === 'text') e.textContent = v;
        else if (k.startsWith('data-')) e.setAttribute(k, v);
        else e[k] = v;
      }
    }
    for (const c of children) {
      if (c === null || c === undefined) continue;
      e.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return e;
  }

  function show(id, on) {
    $(id).hidden = !on;
  }

  // Mutações levam Origin (referrerPolicy same-origin: com no-referrer o navegador mandaria Origin: null),
  // X-CB-Admin e JSON. Cookies só same-origin.
  async function api(method, path, body) {
    const opt = { method, credentials: 'same-origin', cache: 'no-store', referrerPolicy: 'same-origin', headers: {} };
    if (method !== 'GET') {
      opt.headers['Content-Type'] = 'application/json';
      opt.headers['X-CB-Admin'] = '1';
      opt.body = JSON.stringify(body || {});
    }
    let r;
    try {
      r = await fetch(path, opt);
    } catch (e) {
      return { status: 0, data: { error: 'servidor inacessível' } };
    }
    let data = null;
    try {
      data = await r.json();
    } catch (e) {
      data = null;
    }
    if (r.status === 401 && path.startsWith('/api/') && path !== '/api/me') {
      stopTimers();
      await boot();
    }
    return { status: r.status, data };
  }

  function errText(r) {
    return (r.data && typeof r.data.error === 'string') ? r.data.error : 'erro ' + r.status;
  }

  let flashTimer = null;
  function flash(msg, bad) {
    const f = $('flash');
    f.textContent = msg;
    f.className = bad ? 'flash bad' : 'flash';
    f.hidden = false;
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => { f.hidden = true; }, 4000);
  }

  // ---------------------------------------------------------------- login

  async function loginView(error) {
    stopTimers();
    show('view-loading', false);
    show('view-app', false);
    show('who', false);
    show('view-login', true);
    const err = $('login-error');
    err.hidden = !error;
    err.textContent = error || '';
    const r = await api('GET', '/auth/mode');
    const m = r.data || {};
    if (m.serverId) $('server-id').textContent = m.serverId;
    show('login-hub', !!m.hub);
    show('login-dev', !!m.dev);
    let msg = '';
    if (m.hub) msg = 'Você será levado ao Hub para confirmar sua conta Discord.';
    else if (m.hubConfigured) msg = 'Login via Hub indisponível: configure admin.publicUrl no server.cfg (endereço deste painel visto pelo navegador).';
    else if (!m.dev) msg = 'Login indisponível: servidor sem Hub e sem admin.devToken.';
    $('login-msg').textContent = msg;
  }

  async function handleCallback() {
    const params = new URLSearchParams(location.hash.slice(1));
    const ticket = params.get('ticket');
    const state = params.get('state');
    history.replaceState(null, '', '/auth/callback'); // o ticket sai da barra de endereço e do histórico
    if (!ticket || !state) {
      await loginView('Retorno do Hub sem ticket ou state. Comece de novo.');
      return;
    }
    const r = await api('POST', '/auth/login', { ticket, state });
    if (r.status === 200) {
      location.replace('/');
      return;
    }
    history.replaceState(null, '', '/');
    await loginView(errText(r));
  }

  $('login-dev').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const r = await api('POST', '/auth/login', { devToken: $('dev-token').value });
    $('dev-token').value = '';
    if (r.status === 200) await boot();
    else await loginView(errText(r));
  });

  $('logout').addEventListener('click', async () => {
    await api('POST', '/auth/logout', {});
    await loginView();
  });

  // ---------------------------------------------------------------- abas

  let current = 'players';
  let timer = null;
  const refreshers = {};
  const periods = { players: 3000, resources: 3000, logs: 5000, metrics: 2000 };

  function stopTimers() {
    clearInterval(timer);
    timer = null;
  }

  function selectTab(name) {
    current = name;
    for (const b of document.querySelectorAll('.tabs button')) b.classList.toggle('active', b.dataset.tab === name);
    for (const t of document.querySelectorAll('.tab')) t.hidden = t.id !== 'tab-' + name;
    stopTimers();
    const f = refreshers[name];
    if (f) {
      f();
      if (periods[name]) timer = setInterval(f, periods[name]);
    }
    if (name === 'console') $('console-line').focus();
  }

  for (const b of document.querySelectorAll('.tabs button')) b.addEventListener('click', () => selectTab(b.dataset.tab));

  // ---------------------------------------------------------------- jogadores

  async function playerAction(p, action) {
    let body = {};
    if (action === 'kick') {
      const reason = prompt('Motivo do kick de ' + p.name + ':', '');
      if (reason === null) return;
      body = { reason };
    } else if (action === 'ban') {
      const reason = prompt('Motivo do ban de ' + p.name + ' (' + p.did + '):', '');
      if (reason === null) return;
      const hours = prompt('Duração em horas (0 = permanente):', '0');
      if (hours === null) return;
      const h = Number(hours);
      if (!Number.isFinite(h) || h < 0) { flash('Duração inválida', true); return; }
      body = { reason, durationS: Math.round(h * 3600) };
    } else {
      const pos = prompt('Teleportar ' + p.name + ' para "x y z" (cm):', p.x + ' ' + p.y + ' ' + p.z);
      if (pos === null) return;
      const v = pos.trim().split(/\s+/).map(Number);
      if (v.length !== 3 || v.some((n) => !Number.isInteger(n))) { flash('Use três inteiros: x y z', true); return; }
      body = { x: v[0], y: v[1], z: v[2] };
    }
    const r = await api('POST', '/api/players/' + encodeURIComponent(p.id) + '/' + action, body);
    if (r.status === 202) flash(action + ' enviado: ' + p.name);
    else flash(errText(r), true);
    refreshers.players();
  }

  refreshers.players = async () => {
    const r = await api('GET', '/api/players');
    if (r.status !== 200 || !Array.isArray(r.data)) return;
    const body = $('players-body');
    body.replaceChildren(...r.data.map((p) => el('tr', null,
      el('td', { class: 'num', text: p.id }),
      el('td', { text: p.name }),
      el('td', { class: 'mono', text: p.did }),
      el('td', { class: 'mono', text: p.x + ', ' + p.y + ', ' + p.z }),
      el('td', { class: 'actions' },
        el('button', { type: 'button', text: 'Kick', onclick: () => playerAction(p, 'kick') }),
        el('button', { type: 'button', text: 'Ban', class: 'danger', onclick: () => playerAction(p, 'ban') }),
        el('button', { type: 'button', text: 'Teleporte', onclick: () => playerAction(p, 'teleport') })))));
    $('players-count').textContent = '(' + r.data.length + ')';
    show('players-empty', r.data.length === 0);
  };

  // ---------------------------------------------------------------- resources

  async function resourceAction(name, op) {
    if (op !== 'start' && !confirm(op + ' ' + name + '?')) return;
    const r = await api('POST', '/api/resources/' + encodeURIComponent(name) + '/' + op, {});
    if (r.status === 200) flash(name + ': ' + op + (r.data.ok ? ' ok' : ' falhou'), !r.data.ok);
    else if (r.status === 202) flash(name + ': ' + op + ' em andamento');
    else flash(errText(r), true);
    refreshers.resources();
  }

  function bytes(n) {
    if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + ' MiB';
    if (n >= 1 << 10) return (n / (1 << 10)).toFixed(0) + ' KiB';
    return n + ' B';
  }

  refreshers.resources = async () => {
    const r = await api('GET', '/api/resources');
    if (r.status !== 200 || !Array.isArray(r.data)) return;
    $('resources-body').replaceChildren(...r.data.map((x) => el('tr', null,
      el('td', { text: x.name }),
      el('td', null, el('span', { class: 'pill ' + x.state, text: x.state })),
      el('td', { class: 'num', text: Number(x.cpuMsPerSec).toFixed(1) }),
      el('td', { class: 'num', text: bytes(Number(x.memBytes)) }),
      el('td', { class: 'err', text: x.lastError || '' }),
      el('td', { class: 'actions' },
        el('button', { type: 'button', text: 'Iniciar', onclick: () => resourceAction(x.name, 'start') }),
        el('button', { type: 'button', text: 'Parar', onclick: () => resourceAction(x.name, 'stop') }),
        el('button', { type: 'button', text: 'Reiniciar', onclick: () => resourceAction(x.name, 'restart') })))));
  };

  // ---------------------------------------------------------------- console

  function consoleWrite(text, cls) {
    const out = $('console-out');
    out.append(el('span', { class: cls || '', text: text + '\n' }));
    while (out.childNodes.length > 400) out.removeChild(out.firstChild);
    out.scrollTop = out.scrollHeight;
  }

  $('console-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const input = $('console-line');
    const line = input.value.trim();
    if (!line) return;
    input.value = '';
    consoleWrite('> ' + line, 'cmd');
    const r = await api('POST', '/api/console', { line });
    if (r.status !== 200) consoleWrite(errText(r), 'bad');
    else if (r.data.pending) consoleWrite('(sem resposta ainda; veja o log)', 'muted');
    else consoleWrite(r.data.reply === null ? '' : String(r.data.reply));
  });

  // ---------------------------------------------------------------- logs

  refreshers.logs = async () => {
    const kind = $('logs-kind').value;
    const tail = $('logs-tail').value;
    const r = await api('GET', '/api/logs?kind=' + encodeURIComponent(kind) + '&tail=' + encodeURIComponent(tail));
    if (r.status !== 200) return;
    $('logs-file').textContent = r.data.file ? 'Arquivo: ' + r.data.file : 'Nenhum arquivo ainda.';
    const out = $('logs-out');
    const atBottom = out.scrollTop + out.clientHeight >= out.scrollHeight - 4;
    out.textContent = (r.data.lines || []).join('\n');
    if (atBottom) out.scrollTop = out.scrollHeight;
  };
  $('logs-kind').addEventListener('change', () => refreshers.logs());
  $('logs-tail').addEventListener('change', () => refreshers.logs());

  // ---------------------------------------------------------------- métricas

  refreshers.metrics = async () => {
    const r = await api('GET', '/api/metrics');
    if (r.status !== 200 || !r.data) return;
    const rows = [];
    const add = (prefix, obj) => {
      for (const [k, v] of Object.entries(obj)) {
        if (v && typeof v === 'object' && !Array.isArray(v)) add(prefix + k + '.', v);
        else rows.push(el('tr', null, el('th', { text: prefix + k }),
          el('td', { class: 'num', text: typeof v === 'number' && !Number.isInteger(v) ? v.toFixed(2) : JSON.stringify(v) })));
      }
    };
    add('', r.data);
    $('metrics-body').replaceChildren(...rows);
  };

  // ---------------------------------------------------------------- início

  async function boot() {
    const me = await api('GET', '/api/me');
    if (me.status !== 200) {
      await loginView();
      return;
    }
    $('server-id').textContent = me.data.serverId || '';
    $('who-name').textContent = me.data.name + ' (' + me.data.did + ')';
    show('who', true);
    show('view-loading', false);
    show('view-login', false);
    show('view-app', true);
    selectTab(current);
  }

  if (location.pathname === '/auth/callback') handleCallback();
  else boot();
})();
