"use strict";

// ---------- 分析模式（编辑 + 全局分析） ----------
// 与游玩模式共享全局状态（state/prob/hover/settings 等，由 main.js 声明）。
// 本文件只负责分析模式的编辑与全局分析 UI，纯前端交互逻辑，不碰算法层。
//
// 交互模型：进入分析模式即自动开始持续分析（开始分析/结束分析切换），
// 编辑后防抖自动重算；持续期间周期性重算（后端 anytime 会话继续生长）。

let analyzer = false;   // 分析模式：保留盘面，禁止改盘操作
let analyzing = false;  // 持续分析中：编辑自动重算 + 周期加深
let analyzeBusy = false;
let analyzeTimer = null;
let analyzeLoop = null;
let analyzeBudget = 128;      // 每次分析的服务端节点预算（自适应）
const ANALYZE_TARGET_MS = 100;  // 每次分析目标耗时（服务端 ms 字段实测校准）
const ANALYZE_INTERVAL_MS = 1000;
let prevProb = false;   // 进入分析前的概率显示状态（退出时恢复）
let editBoard = null;   // 分析模式的编辑副本（进入时复制当前盘面）

// 当前渲染盘面格值：分析模式用编辑副本，否则用游戏盘面。
function boardAt(i, j) {
  return editBoard ? editBoard[i][j] : state.board[i][j];
}

// 进入分析模式：快照盘面 + 显示概率 + 开启编辑 + 自动开始持续分析。
async function enterAnalyzer(btn) {
  analyzer = true;
  btn.classList.add("active");
  btn.textContent = "退出分析";
  await post("/api/analyzer", { active: true });  // 服务端快照原始盘面
  editBoard = state.board.map((row) => row.slice());  // 本地编辑副本
  document.getElementById("analyze-section").hidden = false;
  prevProb = settings.prob;
  settings.prob = true;
  document.getElementById("opt-prob").checked = true;
  await refreshProb();
  render();
  startAnalysis();
}

// 退出分析模式：还原盘面 + 恢复概率状态 + 隐藏分析区 + 结束持续分析。
async function exitAnalyzer(btn) {
  stopAnalysis();
  analyzer = false;
  btn.classList.remove("active");
  btn.textContent = "分析";
  editBoard = null;
  document.getElementById("analyze-section").hidden = true;
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
  if (!analyzer) return;
  analyzer = false;
  editBoard = null;
  const abtn = document.getElementById("analyzer-btn");
  abtn.classList.remove("active");
  abtn.textContent = "分析";
  document.getElementById("analyze-section").hidden = true;
}

// 编辑一格：更新本地编辑副本 + 同步后端分析视图。
// 后端协议 v=0..8 数字、v=9 盖上（Hidden）；本地 editBoard 用 -1 表示封闭。
// 编辑后防抖自动重算（持续分析开启时）。
async function editCell(x, y, next) {
  editBoard[x - 1][y - 1] = next;
  prob = null;
  render();
  try {
    await post("/api/edit", { x, y, v: next === -1 ? 9 : next });
  } catch (_) { /* 忽略瞬时错误 */ }
  scheduleAnalyze();
}

// 分析模式悬停详情：从本地编辑盘面 + 最近一次分析的概率网格构建（不发请求）。
// prob 为空（编辑后未分析）时只给状态，提示分析进行中。
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

// 跑一次全局分析：合法性检查 + 暴力求解/中盘搜索（后端 anytime 会话持续生长）。
// 预算自适应：按上次实测 ms 调整节点数，目标每次 ~ANALYZE_TARGET_MS。
async function runAnalyze() {
  if (analyzeBusy) return;
  analyzeBusy = true;
  const el = document.getElementById("analyze-text");
  el.textContent = "分析中…";
  try {
    const data = await post("/api/analyze", { budget: analyzeBudget });
    if (data.ms > 0) {
      analyzeBudget = Math.max(128, Math.min(8192,
        Math.round(analyzeBudget * ANALYZE_TARGET_MS / data.ms)));
    }
    // 用本次重构的精确概率网格刷新盘面覆盖（编辑后旧概率已失效）
    if (data.prob) {
      prob = {
        prob: data.prob,
        tProb: data.tProb,
        candidates: data.candidates,
        computedMs: data.ms,
      };
      render();
    }
    const lines = [];
    if (!data.valid) {
      lines.push("盘面不合法：");
      lines.push(data.reason || "未知原因");
    } else if (data.midgame) {
      lines.push("候选方案数: " + data.candidates);
      lines.push("超过暴力阈值，中盘搜索");
      if (data.firstMove && data.firstMove[0] > 0) {
        lines.push("最优首招: (" + data.firstMove[0] + ", " + data.firstMove[1] + ")");
        lines.push("比较深度: " + data.searchDepth);
        lines.push("期望死亡: " + (data.value * 100).toFixed(2) + "%");
        lines.push("搜索节点: " + data.searchNodes + "，observe: " + data.searchObserves);
      } else {
        lines.push("无可分析格子");
      }
      lines.push("计算耗时: " + data.ms + " ms");
    } else if (data.bruteforce) {
      lines.push("候选方案数: " + data.candidates);
      lines.push("暴力枚举: " + data.total + " 个方案");
      if (data.firstMove && data.firstMove[0] > 0) {
        lines.push("最优首招: (" + data.firstMove[0] + ", " + data.firstMove[1] + ")");
        lines.push("可保证赢下: " + data.wins + " / " + data.total);
        lines.push("胜率: " + data.winRate.toFixed(2) + "%");
      } else {
        lines.push("无可分析格子");
      }
      lines.push("DFS 节点: " + data.nodes);
      lines.push("计算耗时: " + data.ms + " ms");
    } else {
      lines.push("候选方案数: " + data.candidates);
      lines.push("（未执行分析）");
    }
    el.textContent = lines.join("\n");
  } catch (_) {
    el.textContent = "分析失败（服务异常）";
  } finally {
    analyzeBusy = false;
  }
}

// 持续分析：立即跑一次 + 周期性加深（后端 anytime 会话复用，树持续生长）。
function startAnalysis() {
  if (analyzing) return;
  analyzing = true;
  document.getElementById("analyze-btn").textContent = "结束分析";
  runAnalyze();
  clearInterval(analyzeLoop);
  analyzeLoop = setInterval(runAnalyze, ANALYZE_INTERVAL_MS);
}

// 结束持续分析：停周期与防抖，保留当前结果。
function stopAnalysis() {
  analyzing = false;
  clearTimeout(analyzeTimer);
  clearInterval(analyzeLoop);
  analyzeLoop = null;
  document.getElementById("analyze-btn").textContent = "开始分析";
}

// 编辑后防抖自动重算（仅持续分析开启时）。
function scheduleAnalyze() {
  if (!analyzing) return;
  clearTimeout(analyzeTimer);
  analyzeTimer = setTimeout(runAnalyze, 300);
}

// 分析按钮：一键切换到分析页面（保留当前盘面，不开始计算）。
// 进入：快照盘面 + 显示概率 + 开启编辑 + 自动开始持续分析；
// 退出：还原盘面 + 恢复概率状态 + 隐藏分析区 + 结束持续分析。
document.getElementById("analyzer-btn").addEventListener("click", async () => {
  const btn = document.getElementById("analyzer-btn");
  if (analyzer) await exitAnalyzer(btn);
  else await enterAnalyzer(btn);
});

// 开始分析/结束分析：切换持续分析（编辑后自动重算，周期加深）。
document.getElementById("analyze-btn").addEventListener("click", () => {
  if (analyzing) stopAnalysis();
  else startAnalysis();
});