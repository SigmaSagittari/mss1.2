"use strict";

const canvas = document.getElementById("main-canvas");
const ctx = canvas.getContext("2d");

const MODES = {
  beg: { rows: 9, cols: 9, mines: 10 },
  int: { rows: 16, cols: 16, mines: 40 },
  exp: { rows: 16, cols: 30, mines: 99 },
};

// 经典扫雷数字颜色
const NUMBER_COLORS = [
  "", "#0000ff", "#008000", "#ff0000", "#000080",
  "#800000", "#008080", "#000000", "#808080",
];

let state = null;
let prob = null;
let analysisBest = null;  // 分析模式最优首招高亮（1-based）
let hover = null;       // {x, y} 1-based
let pressing = false;   // 左键按下中
let currentMode = "beg";
let timerHandle = null;
let startMs = null;
let detailTimer = null;
const detailCache = new Map();  // "x,y" -> 详情文本（盘面变化时清空）
let prevEffects = new Set();    // 当前已绘制的按压效果格子（一维索引）

const settings = {
  prob: false,
  moves: false,   // 招法质量标注（取代概率）：质量/不稳定度/搜索节点数
  pressPreview: true,
  edgeHighlight: true,  // 0% 绿 / 100% 红 醒目标记（代替文字）
  cellPx: 24,   // 格子边长（正方形），可在设置里手动调整
  viewCells: 30,  // 棋盘展示窗口大小（格），超出后棋盘区域滚动
  structMode: "update",  // update = 结构增量更新（默认）；rebuild = 结构全量重建
  debugDump: false,  // 控制台输出调试信息（默认关闭）
  memLimitMb: 1024,  // 后台搜索树内存上限（MB）
};

let moves = null;   // 招法质量标注数据（/api/analyze/moves 结果：cell -> {qual, tO, nodes}）
let movesMap = {};  // cellId -> 标注对象

// 画布布局：格子始终为 cellPx × cellPx 的正方形，
// 棋盘尺寸 = cols×cellPx × rows×cellPx，随难度与设置动态伸缩
let cell = 24;
let W = 0;
let H = 0;

function setupCanvas() {
  const { rows, cols } = state;
  cell = Math.max(8, Math.min(80, settings.cellPx));
  applyViewVars();
  W = cell * cols;
  H = cell * rows;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(W * dpr);
  canvas.height = Math.round(H * dpr);
  canvas.style.width = W + "px";
  canvas.style.height = H + "px";
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
}

// 棋盘展示窗口：把格子边长与“棋盘展示 N 格”同步给 CSS（--cell-px / --view-px）
function applyViewVars() {
  const root = document.documentElement;
  root.style.setProperty("--cell-px", cell + "px");
  root.style.setProperty("--view-px", (settings.viewCells * cell) + "px");
}

// ---------- API ----------
async function api(path, options) {
  const res = await fetch(path, options);
  if (!res.ok) throw new Error("HTTP " + res.status);
  return res.json();
}
const post = (path, body) => api(path, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(body),
});

// 改变盘面的请求必须按用户操作顺序串行发送：
// 服务端是单线程、每个请求走独立连接，并发发送时到达顺序不保证，
// 会出现“旗子请求比 chord 请求晚到 → chord 看到旧旗子而失败”。
let opChain = Promise.resolve();
function postOp(path, body) {
  const run = opChain.then(() => api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  }));
  opChain = run.then(() => {}, () => {});
  return run;
}

// ---------- 生命周期 ----------
async function loadState() {
  state = await api("/api/state");
  setupCanvas();
  const seedEl = document.getElementById("opt-seed");
  if (seedEl) seedEl.value = state.seed;
  render();
}

async function newGame(rows, cols, mines, seed) {
  // 新开局自动退出分析模式（盘面被重置，分析上下文失效）
  resetAnalyzer();
  // seed 缺省 = 随机：服务端生成新随机种子并回填到设置里；给定了就用指定种子
  const body = seed === undefined ? { rows, cols, mines } : { rows, cols, mines, seed };
  state = await postOp("/api/new", body);
  prob = null;
  resetTimer();
  setFace("🙂");
  setupCanvas();
  // 只有随机重开才把新种子回填到设置里；手动指定种子时保持用户输入不被改动
  if (seed === undefined) {
    const seedEl = document.getElementById("opt-seed");
    if (seedEl) seedEl.value = state.seed;
  }
  if (settings.prob) await refreshProb();
  render();
}

async function reveal(x, y) {
  if (startMs === null && state && state.status === "playing") startTimer();
  const prevBoard = state.board;
  state = await postOp("/api/reveal", { x, y });
  if (state.status !== "playing") stopTimer();
  setFace(state.status === "won" ? "😎" : state.status === "lost" ? "😵" : "🙂");
  if (settings.prob) await refreshProb();
  syncStats();
  applyBoardChange(prevBoard);
}

async function toggleFlag(x, y) {
  const prevBoard = state.board;
  state = await postOp("/api/flag", { x, y });
  if (settings.prob) await refreshProb();
  syncStats();
  applyBoardChange(prevBoard);
}

async function refreshProb() {
  // 盘面已变化，旧详情失效
  detailCache.clear();
  try {
    prob = await api("/api/probability");
    // 服务端用真实雷排布核对 0%/100% 判定，不一致时在网页控制台报警
    if (prob && Array.isArray(prob.verify) && prob.verify.length) {
      for (const msg of prob.verify) console.warn("[判定校验] " + msg);
    }
  } catch (_) {
    // 概率不可用（如大棋盘数值溢出）：保持界面可用，详情按需逐格请求
    prob = null;
  }
}

// ---------- 绘制 ----------
function render() {
  if (!state) return;
  syncStats();
  for (let i = 0; i < state.rows; i++)
    for (let j = 0; j < state.cols; j++)
      drawCell(i, j);
  prevEffects = new Set();  // 全盘已按当前状态重画
}

// 只重绘集合里的格子。drawCell 内部做了裁剪，
// 每个格子只画自己的区域，因此无需再外扩邻居。
function redrawCells(cells) {
  for (const k of cells)
    drawCell(Math.floor(k / state.cols), k % state.cols);
}

// 单步操作后的增量重绘：只重绘棋盘值与上次不同的格子。
// 概率显示开启时概率值全盘变化，diff 不再完整，退回整盘重绘。
function applyBoardChange(prevBoard) {
  if (settings.prob && prob) { render(); return; }
  const dirty = new Set();
  for (let i = 0; i < state.rows; i++)
    for (let j = 0; j < state.cols; j++)
      if (prevBoard[i][j] !== state.board[i][j]) dirty.add(i * state.cols + j);
  redrawCells(dirty);
}

// 按住左键预览：悬停未开格 → 该格显示 0；悬停已开格 → 周围未开格显示 0
function isPreview(i, j) {
  if (!settings.pressPreview || !pressing || !hover) return false;
  const hv = state.board[hover.x - 1][hover.y - 1];
  // 被标旗的格子不算"未打开"，不参与预览（v === -2 一律排除）
  if (hv === -1) return i === hover.x - 1 && j === hover.y - 1;
  if (hv >= 0 && hv <= 8) {
    if (state.board[i][j] !== -1) return false;
    const di = Math.abs(i - (hover.x - 1));
    const dj = Math.abs(j - (hover.y - 1));
    return di <= 1 && dj <= 1 && !(di === 0 && dj === 0);
  }
  return false;
}

function drawCell(i, j) {
  const v = boardAt(i, j);
  const px = j * cell;
  const py = i * cell;
  const isHover = hover && hover.x === i + 1 && hover.y === j + 1;
  const preview = isPreview(i, j);

  // 每个格子裁剪到自己的矩形内：浮雕描边、文字等一律画不到邻格，
  // 保证单格重绘自洽，不会在邻格留下残影。
  ctx.save();
  ctx.beginPath();
  ctx.rect(px, py, cell, cell);
  ctx.clip();

  if (preview) {  // 暂时显示为已翻开的 0
    ctx.fillStyle = "#d0d0d0";
    ctx.fillRect(px, py, cell, cell);
    ctx.lineWidth = 1;
    ctx.strokeStyle = "#a0a0a0";
    ctx.strokeRect(px + 0.5, py + 0.5, cell - 1, cell - 1);
  } else if (v === -1 || v === -2) {  // 未翻开 / 标旗
    // 悬停高亮只在按住时出现（未按下没有任何悬停特效）
    ctx.fillStyle = (isHover && pressing) ? "#d8d8d8" : "#c0c0c0";
    ctx.fillRect(px, py, cell, cell);
    // 经典凸起按钮边框
    ctx.lineWidth = Math.max(1, Math.round(cell * 0.08));
    ctx.strokeStyle = "#ffffff";
    ctx.beginPath();
    ctx.moveTo(px, py + cell); ctx.lineTo(px, py); ctx.lineTo(px + cell, py);
    ctx.stroke();
    ctx.strokeStyle = "#808080";
    ctx.beginPath();
    ctx.moveTo(px + cell, py); ctx.lineTo(px + cell, py + cell); ctx.lineTo(px, py + cell);
    ctx.stroke();
    ctx.strokeStyle = "#dfdfdf";
    ctx.beginPath();
    ctx.moveTo(px + cell - 1, py + 1); ctx.lineTo(px + 1, py + 1); ctx.lineTo(px + 1, py + cell - 1);
    ctx.stroke();
    if (v === -2) drawFlag(px, py);
  } else {  // 已翻开
    ctx.fillStyle = "#d0d0d0";
    ctx.fillRect(px, py, cell, cell);
    ctx.lineWidth = 1;
    ctx.strokeStyle = "#a8a8a8";
    ctx.strokeRect(px + 0.5, py + 0.5, cell - 1, cell - 1);
    if (v >= 1 && v <= 8) {
      ctx.fillStyle = NUMBER_COLORS[v];
      ctx.font = "bold " + Math.round(cell * 0.58) + "px Tahoma";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(String(v), px + cell / 2, py + cell / 2 + 1);
    } else if (v === -3 || v === -4) {
      if (v === -4) {  // 引爆点红底
        ctx.fillStyle = "#ff0000";
        ctx.fillRect(px, py, cell, cell);
    }
      drawMine(px, py);
    }
  }

  // 概率显示（只画在未翻开的格子上；预览态不叠加）
  if (!preview && (v === -1 || v === -2)) {
    const mv = movesMap[(i + 1) * (state.cols + 1) + (j + 1)];
    if (settings.moves && mv) {
      drawMovesText(px, py, mv);   // 招法质量标注（取代概率）
    } else if (settings.prob && prob) {
      const p = prob.prob[i][j];
      if (settings.edgeHighlight && p <= 0)
        drawEdgeMark(px, py, "#00b050");   // 安全：醒目绿
      else if (settings.edgeHighlight && p >= 1)
        drawEdgeMark(px, py, "#e00000");   // 雷：醒目红
      else if (p > 0)
        drawProbText(px, py, p);           // 中间概率：百分比文字
    }
  }

  // 分析模式：悬停格画蓝框，标明「悬停 + 按数字键」会改哪格
  if (analyzer && isHover) {
    ctx.lineWidth = 2;
    ctx.strokeStyle = "#0000ff";
    ctx.strokeRect(px + 1, py + 1, cell - 2, cell - 2);
  }

    // 分析模式：最优首招高亮金色粗框
    if (analyzer && analysisBest && analysisBest.x === i + 1 && analysisBest.y === j + 1 &&
        (v === -1 || v === -2)) {
      ctx.lineWidth = Math.max(3, Math.round(cell * 0.12));
      ctx.strokeStyle = "#ffb000";
      ctx.strokeRect(px + 1.5, py + 1.5, cell - 3, cell - 3);
    }

  ctx.restore();
}

function drawProbText(px, py, p) {
  const decimals = cell >= 34 ? 1 : 0;
  const text = (p >= 1 ? 100 : p * 100).toFixed(decimals) + "%";
  ctx.fillStyle = "rgba(0,0,0,0.8)";
  ctx.font = "bold " + Math.max(8, Math.round(cell * 0.22)) + "px 'Courier New'";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(text, px + cell / 2, py + cell / 2 + 1);
}

// 招法质量标注（围棋风格，取代概率）：第一行质量（存活概率%），第二行该格争议度
// （点开后各局面争议 t 的 observe 加权 tO），第三行该招法子树搜索节点数。
function drawMovesText(px, py, mv) {
  const fs = Math.max(8, Math.round(cell * 0.22));
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = "rgba(0,0,0,0.8)";
  ctx.font = "bold " + fs + "px 'Courier New'";
  ctx.fillText((mv.qual * 100).toFixed(0) + "%", px + cell / 2, py + cell * 0.26);
  ctx.font = (fs - 1) + "px 'Courier New'";
  ctx.fillStyle = "rgba(0,0,0,0.6)";
  ctx.fillText(mv.tO.toFixed(2), px + cell / 2, py + cell * 0.55);
  ctx.fillText(String(mv.nodes), px + cell / 2, py + cell * 0.84);
}

function drawEdgeMark(px, py, color) {
  // 淡色底 + 粗边框，醒目但不遮挡旗子
  ctx.fillStyle = color + "30";
  ctx.fillRect(px, py, cell, cell);
  ctx.lineWidth = Math.max(2, Math.round(cell * 0.09));
  ctx.strokeStyle = color;
  ctx.strokeRect(px + 1, py + 1, cell - 2, cell - 2);
}

function drawFlag(px, py) {
  const c = cell;
  ctx.strokeStyle = "#000";
  ctx.lineWidth = Math.max(1, c * 0.06);
  ctx.beginPath();
  ctx.moveTo(px + c * 0.26, py + c * 0.2);
  ctx.lineTo(px + c * 0.26, py + c * 0.82);
  ctx.stroke();
  ctx.fillStyle = "#d00";
  ctx.beginPath();
  ctx.moveTo(px + c * 0.26, py + c * 0.2);
  ctx.lineTo(px + c * 0.66, py + c * 0.3);
  ctx.lineTo(px + c * 0.26, py + c * 0.4);
  ctx.closePath();
  ctx.fill();
}

function drawMine(px, py) {
  const c = cell;
  const cx = px + c / 2;
  const cy = py + c / 2;
  const r = c * 0.32;
  ctx.fillStyle = "#111";
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = "#fff";
  ctx.beginPath();
  ctx.arc(cx - r * 0.35, cy - r * 0.35, r * 0.22, 0, Math.PI * 2);
  ctx.fill();
}

// ---------- 鼠标交互 ----------
function cellAt(e) {
  if (!state) return null;
  const rect = canvas.getBoundingClientRect();
  const mx = e.clientX - rect.left;
  const my = e.clientY - rect.top;
  const j = Math.floor(mx / cell);
  const i = Math.floor(my / cell);
  if (i < 0 || i >= state.rows || j < 0 || j >= state.cols) return null;
  return { x: i + 1, y: j + 1 };
}

// 按压时外观会变化的格子（未按下时没有任何悬停特效）：
//   - 悬停在未翻开/旗格：该格显示按压高亮；
//   - 悬停在数字格：周围未翻开格显示 chord 预览（暂时显示为 0）。
// 返回一维索引集合，用于只重绘受影响格子，避免整盘重绘。
function pressEffects() {
  const cells = new Set();
  if (!state || !settings.pressPreview || !pressing || !hover) return cells;
  const i = hover.x - 1;
  const j = hover.y - 1;
  const v = state.board[i][j];
  const add = (a, b) => {
    if (a >= 0 && a < state.rows && b >= 0 && b < state.cols)
      cells.add(a * state.cols + b);
  };
  if (v === -1 || v === -2) {
    add(i, j);
  } else if (v >= 0 && v <= 8) {
    for (let di = -1; di <= 1; di++)
      for (let dj = -1; dj <= 1; dj++) {
        const ni = i + di, nj = j + dj;
        if (ni >= 0 && ni < state.rows && nj >= 0 && nj < state.cols &&
            state.board[ni][nj] === -1)
          add(ni, nj);
    }
  }
  return cells;
}

// 按压效果统一入口：旧集合 ∪ 新集合一起重绘（含边缘一圈），再记住新集合。
// 按下、移动、抬起、离开画布都走这里，保证预览与绘制状态始终一致。
function updatePressEffects() {
  const next = pressEffects();
  const dirty = new Set(prevEffects);
  for (const k of next) dirty.add(k);
  redrawCells(dirty);
  prevEffects = next;
}

canvas.addEventListener("contextmenu", (e) => e.preventDefault());

canvas.addEventListener("mousedown", (e) => {
  if (analyzer) {
    // 分析编辑：左键切换打开/关闭。数字点一下盖上；盖上（除旗）点一下变 0。
    if (e.button !== 0) return;
    const c = cellAt(e);
    if (!c) return;
    const v = boardAt(c.x - 1, c.y - 1);
    if (v === -2) return;  // 旗格不参与编辑
    const next = (v === -1 || v === -3 || v === -4) ? 0 : -1;
    editCell(c.x, c.y, next);
    return;
  }
  if (e.button === 0) {
    pressing = true;
    hover = cellAt(e);
    updatePressEffects();
  } else if (e.button === 2) {
    // 右键：立即标旗/取消旗
    const c = cellAt(e);
    if (c) toggleFlag(c.x, c.y);
  }
});

canvas.addEventListener("mouseup", (e) => {
  // 只有左键抬起才结束按压并判定点击；右键抬起不能清除 pressing，
  // 否则“按住左键 → 右键插旗 → 松开左键”的 chord 手势会在右键抬起时
  // 丢失按压状态，导致左键抬起后 chord 请求根本不会发出。
  if (e.button !== 0) return;
  const wasPressing = pressing;
  pressing = false;
  updatePressEffects();  // 先撤销预览/高亮，再发点击请求
  if (!wasPressing) return;
  // A 按下拖到 B 抬起 → 判定点击 B（含 chord：点击已开格）
  const c = cellAt(e);
  if (c) reveal(c.x, c.y);
});

canvas.addEventListener("mousemove", (e) => {
  const oldHover = hover;
  hover = cellAt(e);
  const moved = !oldHover || !hover || oldHover.x !== hover.x || oldHover.y !== hover.y;
  if (moved && settings.prob) scheduleDetail(hover);
  if (!pressing || !moved) return;  // 未按下：没有任何悬停特效，无需重绘
  updatePressEffects();
});

canvas.addEventListener("mouseleave", () => {
  hover = null;
  pressing = false;
  updatePressEffects();
});

// ---------- 详细信息面板 ----------
function scheduleDetail(c) {
  clearTimeout(detailTimer);
  const el = document.getElementById("detail-text");
  if (analyzer) {
    // 分析模式：详情从本地编辑盘面 + 最近一次分析的概率网格构建（不发请求）
    const text = c ? analyzerDetailText(c) : "分析模式：点击切换开/关，悬停按 0~8 设数字";
    if (el.textContent !== text) el.textContent = text;
    return;
  }
  if (!c) {
    if (el.textContent !== "悬停在格子上查看") el.textContent = "悬停在格子上查看";
    return;
  }
  // 优先使用本地缓存；未命中才按格请求（50ms 防抖 + 离开该格不写面板）
  const key = c.x + "," + c.y;
  const cached = detailCache.get(key);
  if (typeof cached === "string" && cached) {
    if (el.textContent !== cached) el.textContent = cached;
    return;
  }
  detailTimer = setTimeout(async () => {
    try {
      const res = await fetch("/api/detail?x=" + c.x + "&y=" + c.y);
      const data = await res.json();
      detailCache.set(key, data.text);
      if (hover && hover.x === c.x && hover.y === c.y) el.textContent = data.text;
    } catch (_) { /* 忽略瞬时错误 */ }
  }, 50);
}

// ---------- 状态栏 / 计时 ----------
function timerText() {
  if (startMs === null) return "0:00";
  const s = Math.floor((Date.now() - startMs) / 1000);
  return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
}
function startTimer() {
  startMs = Date.now();
  timerHandle = setInterval(() => {
    document.getElementById("timer").textContent = timerText();
  }, 500);
}
function stopTimer() {
  if (timerHandle) clearInterval(timerHandle);
  timerHandle = null;
}
function resetTimer() {
  stopTimer();
  startMs = null;
  document.getElementById("timer").textContent = "0:00";
}
function setFace(f) {
  document.getElementById("face").textContent = f;
}
function syncStats() {
  document.getElementById("mines-left").textContent = state ? state.flagsRemaining : 0;
}

// ---------- 工具栏 / 设置 ----------
document.querySelectorAll(".mode").forEach((btn) => {
  btn.addEventListener("click", async () => {
    currentMode = btn.dataset.mode;
    document.querySelectorAll(".mode").forEach((b) => b.classList.toggle("active", b === btn));
    document.getElementById("custom-fields").hidden = currentMode !== "cus";
    const m = MODES[currentMode];
    if (m) await newGame(m.rows, m.cols, m.mines);
  });
});

function readCustom() {
  const clamp = (id, lo, hi) => {
    const v = parseInt(document.getElementById(id).value, 10);
    if (isNaN(v)) return lo;
    return Math.min(hi, Math.max(lo, v));
  };
  const rows = clamp("cus-rows", 2, 100);
  const cols = clamp("cus-cols", 2, 100);
  const mines = clamp("cus-mines", 1, rows * cols - 1);
  return { rows, cols, mines };
}

async function restartCurrent() {
  const c = MODES[currentMode] || readCustom();
  await newGame(c.rows, c.cols, c.mines);
}

document.getElementById("cus-start").addEventListener("click", restartCurrent);
document.getElementById("face").addEventListener("click", restartCurrent);

// 空格重新开始（输入框聚焦时不拦截）
window.addEventListener("keydown", (e) => {
  const t = e.target;
  if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA")) return;
  if (analyzer) {
    // 分析编辑：悬停格 + 按 0~8 → 该格变为对应数字
    if (hover && e.key >= "0" && e.key <= "8") editCell(hover.x, hover.y, parseInt(e.key, 10));
    return;  // 分析模式不重开，保留当前盘面
  }
  if (e.code !== "Space" || e.repeat) return;
  e.preventDefault();
  restartCurrent();
});

// 设置二级菜单
const settingsBtn = document.getElementById("settings-btn");
const settingsMenu = document.getElementById("settings-menu");
settingsBtn.addEventListener("click", (e) => {
  e.stopPropagation();
  settingsMenu.hidden = !settingsMenu.hidden;
});
document.addEventListener("click", (e) => {
  if (!settingsMenu.hidden && !settingsMenu.contains(e.target) && e.target !== settingsBtn)
    settingsMenu.hidden = true;
});

document.getElementById("opt-prob").addEventListener("change", async (e) => {
  settings.prob = e.target.checked;
  if (settings.prob) await refreshProb();
  render();
});
document.getElementById("opt-press").addEventListener("change", (e) => {
  settings.pressPreview = e.target.checked;
  render();
});

document.getElementById("opt-moves").addEventListener("change", (e) => {
  settings.moves = e.target.checked;
  render();
});

document.getElementById("opt-edge").addEventListener("change", (e) => {
  settings.edgeHighlight = e.target.checked;
  render();
});

document.getElementById("opt-analysis-mode").addEventListener("change", async (e) => {
  settings.structMode = e.target.value === "rebuild" ? "rebuild" : "update";
  try {
    await post("/api/config", { structMode: settings.structMode });
  } catch (_) { /* 忽略瞬时错误，下次请求会重新同步 */ }
});

// 手动指定种子：用设置里的值开新局（不覆盖输入框，棋盘由该种子决定）
document.getElementById("seed-start").addEventListener("click", async () => {
  const c = MODES[currentMode] || readCustom();
  const v = parseInt(document.getElementById("opt-seed").value, 10);
  await newGame(c.rows, c.cols, c.mines, isNaN(v) ? 0 : v);
});

document.getElementById("opt-cell").addEventListener("change", (e) => {
  const v = parseInt(e.target.value, 10);
  settings.cellPx = isNaN(v) ? 24 : Math.max(8, Math.min(80, v));
  e.target.value = settings.cellPx;
  setupCanvas();
  render();
});

document.getElementById("opt-view").addEventListener("change", (e) => {
  const v = parseInt(e.target.value, 10);
  settings.viewCells = isNaN(v) ? 30 : Math.max(10, Math.min(100, v));
  e.target.value = settings.viewCells;
  applyViewVars();
});

// 搜索树内存上限（MB）：只影响后台分析搜索的封顶，改后下次「开始分析」生效。
document.getElementById("opt-mem").addEventListener("change", (e) => {
  const v = parseInt(e.target.value, 10);
  settings.memLimitMb = isNaN(v) ? 1024 : Math.max(64, Math.min(32768, v));
  e.target.value = settings.memLimitMb;
});

// 设置二级菜单：界面 / 高级
const subButtons = {
  ui: document.getElementById("sub-ui"),
  adv: document.getElementById("sub-adv"),
};
const subPanels = {
  ui: document.getElementById("sub-ui-panel"),
  adv: document.getElementById("sub-adv-panel"),
};
function showSubmenu(name) {
  for (const key of Object.keys(subPanels)) {
    subPanels[key].hidden = key !== name;
    subButtons[key].classList.toggle("active", key === name);
  }
}
subButtons.ui.addEventListener("click", () => showSubmenu("ui"));
subButtons.adv.addEventListener("click", () => showSubmenu("adv"));

// 初始化设置：读取格子大小默认值
const optCell = document.getElementById("opt-cell");
settings.cellPx = parseInt(optCell.value, 10) || 24;
settings.edgeHighlight = document.getElementById("opt-edge").checked;
settings.pressPreview = document.getElementById("opt-press").checked;
settings.moves = document.getElementById("opt-moves").checked;
const optView = document.getElementById("opt-view");
settings.viewCells = parseInt(optView.value, 10) || 30;
const optMem = document.getElementById("opt-mem");
settings.memLimitMb = parseInt(optMem.value, 10) || 1024;
applyViewVars();

// 初始化：与服务器同步结构处理方式设置
async function loadConfig() {
  try {
    const cfg = await api("/api/config");
    settings.structMode = cfg.structMode === "rebuild" ? "rebuild" : "update";
    document.getElementById("opt-analysis-mode").value = settings.structMode;
  } catch (_) { /* 服务器未就绪时保持默认 */ }
}

loadConfig();
loadState();
