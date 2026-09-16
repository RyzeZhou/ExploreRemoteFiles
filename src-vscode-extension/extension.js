// ERF Terminal Bridge —— Explorer 右键「在此打开终端」在 VS Code 侧的执行端点。
//
// 为什么需要它：VS Code 没有「在已连接的窗口里新开一个终端」的命令行接口
// （CLI 只有 --new-window / --reuse-window，且只针对文件/文件夹），
// 扩展 URI（vscode://）在本机 VS Code 1.137 上也不会派发给 handler。
// 因此改用一个本地请求文件 + 本扩展轮询。
//
// 协议（%LOCALAPPDATA%\ExplorerRemoteFs\，Windows 侧；本扩展是 UI 扩展，
// 即使窗口连的是远端也在本地扩展宿主运行，所以路径固定是本机的 LOCALAPPDATA）：
//
//   vscode-request.json   { "authority": "ssh-remote+<别名>", "path": "/远端/目录", "ts": <epoch ms> }
//       → 由资源管理器扩展/CLI 写入；本扩展发现后：authority 与本窗口一致就
//         在本窗口 createTerminal({cwd}) 并删除该文件（一次性消费，TTL 120s）。
//
//   windows\win-<pid>.json  { "pid": <本扩展宿主 pid>, "authority": "ssh-remote+<别名>", "ts": <epoch ms> }
//       → 心跳：每 2 秒刷新；启动器据此判断「是否已有窗口连着这个主机」，
//         有就只写请求（不启动窗口 → 不新开窗口、不再弹工作区信任），
//         没有才启动窗口。退出时删除自己的条目。
//
// 只用稳定 API：vscode.env.remoteAuthority 是提案 API(resolvers)，普通扩展调用会抛
// "CANNOT use API proposal: resolvers"。远端身份从 workspaceFolders[0].uri.authority 取。
const vscode = require('vscode');
const fs = require('fs');
const os = require('os');
const path = require('path');

const BASE = path.join(process.env.LOCALAPPDATA || os.tmpdir(), 'ExplorerRemoteFs');
const REQUEST = path.join(BASE, 'vscode-request.json');
const WINDOW_DIR = path.join(BASE, 'windows');
const HEARTBEAT = path.join(WINDOW_DIR, `win-${process.pid}.json`);
const LOG = path.join(os.tmpdir(), 'erf-vscode-bridge.log');

const REQUEST_TTL_MS = 120000;
const HEARTBEAT_MS = 2000;

let heartbeatTimer = null;

function log(msg) {
    try { fs.appendFileSync(LOG, `[${new Date().toISOString()}] pid=${process.pid} ${msg}\n`); } catch (e) { /* ignore */ }
}

/** 本窗口连的远端，例如 "ssh-remote+erf-WSL-SFTP"；本地窗口返回空串。 */
function currentAuthority() {
    const folders = vscode.workspace.workspaceFolders;
    if (folders && folders.length > 0) return folders[0].uri.authority || '';
    return '';
}

function writeHeartbeat() {
    try {
        if (!fs.existsSync(WINDOW_DIR)) fs.mkdirSync(WINDOW_DIR, { recursive: true });
        fs.writeFileSync(HEARTBEAT,
            JSON.stringify({ pid: process.pid, authority: currentAuthority(), ts: Date.now() }),
            'utf8');
    } catch (e) { /* ignore */ }
}

function removeHeartbeat() {
    try { fs.unlinkSync(HEARTBEAT); } catch (e) { /* ignore */ }
}

/** 终端页签名：只显示最后一级目录名（用户要求；完整路径在终端里 `pwd` 就能看到）。 */
function terminalName(target) {
    const clean = String(target || '/').replace(/\/+$/, '');
    const base = clean.slice(clean.lastIndexOf('/') + 1);
    return 'ERF: ' + (base || clean || '/');
}

function openTerminalAt(target) {
    const auth = currentAuthority();
    const cwd = auth ? vscode.Uri.parse(`vscode-remote://${auth}${target}`) : vscode.Uri.file(target);
    const term = vscode.window.createTerminal({ name: terminalName(target), cwd });
    term.show();
    return cwd;
}

let lastRequest = '';

function pollRequest() {
    let raw;
    try {
        if (!fs.existsSync(REQUEST)) return;
        raw = fs.readFileSync(REQUEST, 'utf8');
    } catch (e) { return; }
    if (!raw || raw === lastRequest) return;
    lastRequest = raw;

    let req;
    try { req = JSON.parse(raw); } catch (e) { log(`bad request json: ${raw}`); return; }

    const auth = currentAuthority();
    const age = Date.now() - (req.ts || 0);
    if (age > REQUEST_TTL_MS) { log(`request expired (${age}ms) -> ignore`); return; }
    if (req.authority && req.authority !== auth) {
        log(`request for ${req.authority}, this window is ${auth || '(local)'} -> ignore`);
        return;
    }

    try {
        const cwd = openTerminalAt(req.path || '/');
        log(`terminal created cwd=${cwd.toString()}`);
        try { fs.unlinkSync(REQUEST); } catch (e) { /* ignore */ }
        lastRequest = '';
    } catch (e) {
        log(`terminal error ${e && e.stack ? e.stack : e}`);
    }
}

function activate(context) {
    log(`activate authority=${currentAuthority() || '(none)'} remoteName=${vscode.env.remoteName || '(none)'} tmppath=${LOG}`);
    writeHeartbeat();
    pollRequest();
    heartbeatTimer = setInterval(() => { writeHeartbeat(); pollRequest(); }, HEARTBEAT_MS);
    context.subscriptions.push({ dispose() { if (heartbeatTimer) clearInterval(heartbeatTimer); removeHeartbeat(); } });

    // 保留 URI handler：当前版本不派发，留着不影响；将来可用则自动生效。
    context.subscriptions.push(vscode.window.registerUriHandler({
        handleUri(uri) {
            const q = new URLSearchParams(uri.query || '');
            const target = q.get('path') || '/';
            log(`uri ${uri.toString()}`);
            try { log(`terminal created (uri) cwd=${openTerminalAt(target).toString()}`); } catch (e) { log(`uri error ${e}`); }
        }
    }));

    context.subscriptions.push(vscode.commands.registerCommand('explorerremotefs.openTerminalAt', (p) => {
        log(`command openTerminalAt ${p}`);
        openTerminalAt(p);
    }));
}

function deactivate() {
    if (heartbeatTimer) clearInterval(heartbeatTimer);
    removeHeartbeat();
}

module.exports = { activate, deactivate };
