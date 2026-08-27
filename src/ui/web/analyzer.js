"use strict";

// ---------- 分析模式（编辑 + 全局分析） ----------
// 与游玩模式共享全局状态（state/prob/hover/settings 等，由 main.js 声明）。
// 本文件只负责分析模式的编辑、开始/停止、状态轮询与搜索树展示。
//
// 交互模型：进入分析模式只进入编辑态，不自动开始分析。
// 点「开始分析」才启动后台搜索线程；编辑后如果正在分析，自动重启分析。

let analyzer = false;   // 分析模式：保留盘面，禁止改盘操作
let analyzing = false;  // 持续分析中：编辑自动重算 + 状态轮询
let analyzeBusy = false;
let analyzeTimer = null;
let analyzeLoop = null;
const ANALYZE_POLL_MS = 200;
let prevProb = false;   // 进入分析前的概率显示状态（退出时恢复）
let editBoard = null;   // 分析模式的编辑副本（进入时复制当前盘面）
let treeCurrent = 0;    // 当前查看的树节点
let treeBusy = false;
let treeCache = {};             // 已加载的节点缓存：id -> 节点对象
let lastBestKey = null;

// 当前渲染盘面格值：分析模式用编辑副本，否则用游戏盘面。
function boardAt(i, j) {
  return editBoard ? editBoard[i][j] : state.board[i][j];
}

// 进入分析模式：快照盘面 + 显示概率 + 开启编辑。不自动开始分析。
async function enterAnalyzer(btn) {
  analyzer = true;
  btn.classList.add("active");
  btn.textContent = "退出分析";
  await post("/api/analyzer", { active: true });  // 服务端快照原始盘面
  editBoard = state.board.map((row) => row.slice());  // 本地编辑副本
  document.getElementById("analyze-section").hidden = false;
  document.getElementById("analyze-tree").hidden = true;
  prevProb = settings.prob;
  settings.prob = true;
  document.getElementById("opt-prob").checked = true;
    await refreshProb();
  render();
  // 不自动 start；等用户点「开始分析」。
}

// 退出分析模式：还原盘面 + 恢复概率状态 + 隐藏分析区 + 结束持续分析。
async function exitAnalyzer(btn) {
  stopAnalysis();
  analyzer = false;
  analysisBest = null;
  lastBestKey = null;
  movesMap = {};
  moves = null;
  btn.classList.remove("active");
  btn.textContent = "分析";
  editBoard = null;
  document.getElementById("analyze-section").hidden = true;
  document.getElementById("analyze-tree").hidden = true;
  settings.prob = prevProb;
  document.getElementById("opt-prob").checked = prevProb;
  prob = null;
  await post("/api/analyzer", { active: false });  // 服务端还原盘面
  state = await api("/api/state");                  // 重新拉取真实盘面
  render();
}

// 新开局重置分析模式（main.js newGame 调用；盘面被重置，分析上下文失效）。
function resetAnalyzer() {
  if (!analyzer && !analyzing) return;
  stopAnalysis();
  analysisBest = null;
  lastBestKey = null;
  movesMap = {};
  moves = null;
  if (!analyzer) return;
  analyzer = false;
  editBoard = null;
  const abtn = document.getElementById("analyzer-btn");
  abtn.classList.remove("active");
  abtn.textContent = "分析";
  document.getElementById("analyze-section").hidden = true;
  document.getElementById("analyze-tree").hidden = true;
}

// 编辑一格：更新本地编辑副本 + 同步后端分析视图。
// 后端协议 v=0..8 数字、v=9 盖上（Hidden）；本地 editBoard 用 -1 表示封闭。
// 编辑后防抖自动重算（持续分析开启时）。
async function editCell(x, y, next) {
  editBoard[x - 1][y - 1] = next;
  prob = null;
  analysisBest = null;
  lastBestKey = null;
  movesMap = {};
  moves = null;
  render();
  try {
    await post("/api/edit", { x, y, v: next === -1 ? 9 : next });
  } catch (_) { /* 忽略瞬时错误 */ }
  scheduleAnalyze();
}

// 分析模式悬停详情：从本地编辑盘面 + 最近一次分析的概率网格构建（不发请求）。
function analyzerDetailText(c) {
  const i = c.x - 1, j = c.y - 1;
  const v = boardAt(i, j);
  let stateLine;
  if (v === -2) stateLine = "已标旗";
  else if (v >= 0 && v <= 8) stateLine = "已翻开，数字 " + v;
  else stateLine = "未翻开";
  const lines = ["格子 (" + c.x + ", " + c.y + ")", "状态: " + stateLine];
  if (prob && prob.prob) {
    const p = prob.prob[i][j];
    lines.push("雷概率: " + (p * 100).toFixed(2) + "%");
  } else {
    lines.push("分析进行中…");
  }
  return lines.join("\n");
}

// 用一次快照刷新分析文字/最优高亮/搜索树。
function updateAnalyzeData(data) {
  if (data && data.valid && data.firstMove && data.firstMove[0] > 0) {
    analysisBest = { x: data.firstMove[0], y: data.firstMove[1] };
  } else {
    analysisBest = null;
  }
  const bestKey = analysisBest ? analysisBest.x + "," + analysisBest.y : "";
  if (bestKey !== lastBestKey) {
    lastBestKey = bestKey;
    render();
  }

  const lines = [];
  if (!data || !data.valid) {
    lines.push("盘面不合法：");
    lines.push((data && data.reason) || "未知原因");
  } else if (data.midgame) {
    lines.push("候选方案数: " + data.candidates);
    lines.push("超过暴力阈值，中盘搜索");
    if (data.running) lines.push("搜索中…");
    if (data.firstMove && data.firstMove[0] > 0) {
      lines.push("最优首招: (" + data.firstMove[0] + ", " + data.firstMove[1] + ")");
      lines.push("比较深度: " + data.searchDepth);
      lines.push("期望死亡: " + (data.value * 100).toFixed(2) + "%");
      lines.push("搜索节点: " + data.searchNodes + "，observe: " + data.searchObserves);
      lines.push("节点速率: " + (data.nodeRate != null ? data.nodeRate.toFixed(1) : "0") + " node/s");
    } else {
      lines.push("尚未产生首招");
    }
      lines.push("累计计算耗时: " + (data.totalMs != null ? data.totalMs : data.ms) + " ms");
    document.getElementById("analyze-tree").hidden = false;
      // 树只在用户点击「刷新/根/分支」时加载，避免阻塞 status 轮询。
  } else if (data.bruteforce) {
    lines.push("候选方案数: " + data.candidates);
    lines.push("暴力枚举: " + data.total + " 个方案");
    if (data.running) lines.push("暴力计算中…");
    if (data.firstMove && data.firstMove[0] > 0) {
      lines.push("最优首招: (" + data.firstMove[0] + ", " + data.firstMove[1] + ")");
      lines.push("可保证赢下: " + data.wins + " / " + data.total);
      lines.push("胜率: " + data.winRate.toFixed(2) + "%");
    } else {
      lines.push("尚未产生首招");
    }
    lines.push("DFS 节点: " + data.nodes);
      lines.push("累计计算耗时: " + (data.totalMs != null ? data.totalMs : data.ms) + " ms");
  } else {
    lines.push("候选方案数: " + data.candidates);
    lines.push("（未执行分析）");
  }
  const txt = lines.join("\n");
  const el = document.getElementById("analyze-text");
  // 文本没变就不重写 DOM：避免每 200ms 刷新打断用户复制/选中。
  if (el.textContent !== txt) el.textContent = txt;
}

// 启动/重启后台分析（编辑后也走这里：后端会先停旧线程再建新会话）。
async function runAnalyze() {
  if (analyzeBusy) return;
  analyzeBusy = true;
  const el = document.getElementById("analyze-text");
  el.textContent = "启动分析…";
  try {
    const data = await post("/api/analyze/start?memLimit=" + settings.memLimitMb, {});
    if (!analyzing) {
      // 等待 start 期间被用户停止：取消刚启动的后台任务。
      post("/api/analyze/stop").catch(() => {});
      return;
    }
    await refreshProb();
    render();
    updateAnalyzeData(data);
  } catch (_) {
    el.textContent = "分析失败（服务异常）";
  } finally {
    analyzeBusy = false;
  }
}

// 持续分析：启动后台线程 + 每 200ms 拉一次轻量状态。
function startAnalysis() {
  if (analyzing) return;
  analyzing = true;
  document.getElementById("analyze-btn").textContent = "结束分析";
  runAnalyze();
  clearInterval(analyzeLoop);
  analyzeLoop = setInterval(fetchAnalyzeStatus, ANALYZE_POLL_MS);
}

// 结束持续分析：停周期与防抖；正在启动时由 runAnalyze 负责取消。
function stopAnalysis() {
  analyzing = false;
  clearTimeout(analyzeTimer);
  clearInterval(analyzeLoop);
  analyzeLoop = null;
  document.getElementById("analyze-btn").textContent = "开始分析";
  if (!analyzeBusy) {
    post("/api/analyze/stop").catch(() => {});
  }
}

async function fetchAnalyzeStatus() {
  if (!analyzing) return;
  try {
    const data = await api("/api/analyze/status");
    updateAnalyzeData(data);
    // 招法质量标注（设置开启时）：拉全量候选的质量/不稳定度/节点数，更新棋盘。
    if (settings.moves) {
      try {
        const mv = await api("/api/analyze/moves");
        if (mv && mv.moves) {
          const map = {};
          for (const m of mv.moves) map[m.cell] = m;
          // 只有数据变化才重绘棋盘（避免每 200ms 全量重绘）。
          if (JSON.stringify(map) !== JSON.stringify(movesMap)) {
            movesMap = map;
            moves = mv;
            render();
          }
        }
      } catch (_) { /* 瞬时错误忽略，下个周期再试 */ }
    }
  } catch (_) { /* 瞬时错误忽略，下个周期再试 */ }
}

// 编辑后防抖自动重算（仅持续分析开启时）。
function scheduleAnalyze() {
  if (!analyzing) return;
  clearTimeout(analyzeTimer);
  analyzeTimer = setTimeout(runAnalyze, 300);
}

// ---------- 搜索树浏览（按需加载 + 本地缓存） ----------
// 不再一次抓 500 节点，也不再显示全局节点 id。
// 点谁就加载谁，已加载的节点会缓存在 treeCache 里；刷新当前会强制重新读后台。
async function refreshTree() {
  treeCurrent = 0;
  await loadTreeNode(0, true);
}

async function loadTreeNode(id, force) {
  if (treeBusy) return;
  treeBusy = true;
  const el = document.getElementById("tree-content");
  try {
    if (!force && treeCache[id]) {
      treeCurrent = id;
      renderTree(treeCache[id]);
      return;
    }
    const data = await api("/api/analyze/tree?node=" + id);
    if (!data || data.error) {
      el.textContent = "（无法加载该节点）";
      return;
    }
    treeCache[data.id] = data;
    treeCurrent = data.id;
    renderTree(data);
  } catch (_) {
    el.textContent = "（搜索树加载失败）";
  } finally {
    treeBusy = false;
  }
}

function renderTree(data) {
  const el = document.getElementById("tree-content");
  if (!data) {
    el.textContent = "（无节点数据）";
    return;
  }
  let html = "<div class='tree-head'>深度 " + data.depth +
    "，value " + data.value.toFixed(4) +
    "，tLocal " + data.tLocal.toFixed(4) +
    "，t " + data.t.toFixed(4) +
    "，C " + data.C.toFixed(4) + "</div>";

  html += "<div class='tree-toolbar'>";
  if (data.parent >= 0) {
    html += "<button type='button' data-tree-action='parent' data-node='" + data.parent + "'>← 返回</button>";
  }
  if (data.id !== 0) {
    html += "<button type='button' data-tree-action='root'>回根</button>";
  }
  html += "<button type='button' data-tree-action='refresh'>刷新当前</button>";
  html += "</div>";

  if (!data.actions || data.actions.length === 0) {
    html += "<div class='tree-action'>（无动作）</div>";
  }
  for (const a of (data.actions || [])) {
    html += "<div class='tree-action'>(" + a.x + ", " + a.y + ") " +
      "p=" + (a.p * 100).toFixed(2) + "% " +
      "r=" + a.r.toFixed(4) + " " +
      "score=" + a.score.toFixed(4) + "</div>";
    for (const b of (a.branches || [])) {
      const probTxt = (b.prob * 100).toFixed(2) + "%";
      if (b.state === "node") {
        const childNodes = b.childNodes != null ? b.childNodes : 0;
        html += "<div class='tree-branch'><button type='button' data-tree-action='node' data-node='" + b.child + "'>" +
          "数字 " + b.digit + " " + probTxt + " → 展开</button>" +
          " <span class='tree-meta'>节点 " + childNodes +
          " · value=" + b.value.toFixed(4) +
          " · t=" + b.t.toFixed(4) +
          " · C=" + b.C.toFixed(4) + "</span></div>";
      } else {
        html += "<div class='tree-branch'>数字 " + b.digit + " " + probTxt +
          " → " + b.state + "</div>";
      }
    }
  }
  el.innerHTML = html;
}

document.getElementById("tree-content").addEventListener("click", (e) => {
  const btn = e.target.closest("[data-tree-action]");
  if (!btn) return;
  const action = btn.dataset.treeAction;
  if (action === "refresh") {
    loadTreeNode(treeCurrent, true);
    return;
  }
  if (action === "root") {
    loadTreeNode(0);
    return;
  }
  if (action === "parent" || action === "node") {
    loadTreeNode(parseInt(btn.dataset.node, 10));
  }
});

// 分析按钮：一键切换到分析页面（保留当前盘面，不开始计算）。
document.getElementById("analyzer-btn").addEventListener("click", async () => {
  const btn = document.getElementById("analyzer-btn");
  if (analyzer) await exitAnalyzer(btn);
  else await enterAnalyzer(btn);
});

// 开始分析/结束分析：只有按钮会启动后台线程。
document.getElementById("analyze-btn").addEventListener("click", () => {
  if (analyzing) stopAnalysis();
  else startAnalysis();
});

document.getElementById("tree-refresh").addEventListener("click", refreshTree);

document.getElementById("tree-root").addEventListener("click", () => {
  loadTreeNode(0);
});