<?php

session_start();

$apiKey = '123456';
$dataFile = __DIR__ . '/sms_data.json';
$logFile = __DIR__ . '/sms_log.txt';
$heartbeatFile = __DIR__ . '/heartbeat.json';

$adminPass = 'admin';

if (isset($_GET['logout'])) {
    unset($_SESSION['sms_admin']);
    header('Location: index.php');
    exit;
}

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['login'])) {
    if ($_POST['password'] === $adminPass) {
        $_SESSION['sms_admin'] = true;
    } else {
        $error = '密码错误';
    }
}

$isLoggedIn = isset($_SESSION['sms_admin']);

$smsData = ['pending' => [], 'sending' => [], 'received' => [], 'sent' => []];
if (file_exists($dataFile)) {
    $smsData = json_decode(file_get_contents($dataFile), true) ?: $smsData;
}

if ($isLoggedIn && $_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['action']) && $_POST['action'] === 'send') {
    $phone = trim($_POST['phone'] ?? '');
    $content = trim($_POST['content'] ?? '');

    if ($phone && $content) {
        $id = 'sms_' . time() . '_' . rand(1000, 9999);
        $smsData['pending'][] = [
            'id' => $id,
            'phone' => $phone,
            'content' => $content,
            'time' => date('Y-m-d H:i:s'),
            'status' => 'pending'
        ];
        file_put_contents($dataFile, json_encode($smsData, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE));
        file_put_contents($logFile, date('Y-m-d H:i:s') . " [发送] {$phone}: {$content}\n", FILE_APPEND);
        $success = '短信已加入发送队列';
    }
}

if ($isLoggedIn && isset($_GET['clear'])) {
    if ($_GET['clear'] === 'received') $smsData['received'] = [];
    if ($_GET['clear'] === 'sent') $smsData['sent'] = [];
    if ($_GET['clear'] === 'pending') $smsData['pending'] = [];
    if ($_GET['clear'] === 'sending') $smsData['sending'] = [];
    file_put_contents($dataFile, json_encode($smsData, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE));
    header('Location: index.php');
    exit;
}

$heartbeat = [];
if (file_exists($heartbeatFile)) {
    $heartbeat = json_decode(file_get_contents($heartbeatFile), true) ?: [];
}

$isOnline = false;
$lastHbTime = 0;
if (!empty($heartbeat['unixtime'])) {
    $lastHbTime = intval($heartbeat['unixtime']);
    $diff = abs(time() - $lastHbTime);
    $isOnline = ($diff < 120);
} elseif (!empty($heartbeat['timestamp'])) {
    $lastHbTime = strtotime($heartbeat['timestamp']);
    $diff = abs(time() - $lastHbTime);
    $isOnline = ($diff < 120);
}

$sendList = [];
foreach ($smsData['pending'] ?? [] as $s) {
    $sendList[] = array_merge($s, ['list_type' => 'pending']);
}
foreach ($smsData['sending'] ?? [] as $s) {
    $sendList[] = array_merge($s, ['list_type' => 'sending']);
}
foreach ($smsData['sent'] ?? [] as $s) {
    $sendList[] = array_merge($s, ['list_type' => 'sent']);
}
usort($sendList, function($a, $b) {
    return strtotime($b['time'] ?? '2000') - strtotime($a['time'] ?? '2000');
});

$receivedList = array_reverse($smsData['received'] ?? []);
?>
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>SMS 远程管理</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;background:#f0f2f5;padding:16px;color:#333}
.container{max-width:1200px;margin:0 auto}
h1{color:#1a1a1a;margin-bottom:16px;font-size:22px}
.card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,0.08)}
.card-title{font-size:15px;font-weight:600;color:#1a1a1a;margin-bottom:12px;display:flex;align-items:center;justify-content:space-between;gap:8px}
.form-group{margin-bottom:10px}
.form-group label{display:block;font-size:12px;color:#666;margin-bottom:4px}
.form-group input,.form-group textarea{width:100%;padding:8px 12px;border:1px solid #e0e0e0;border-radius:8px;font-size:14px}
.form-group textarea{min-height:60px;resize:vertical}
.btn{background:#667eea;color:#fff;border:none;padding:8px 16px;border-radius:8px;font-size:13px;cursor:pointer;text-decoration:none;display:inline-block}
.btn:hover{opacity:0.9}
.btn-danger{background:#ff4d4f;font-size:12px;padding:4px 10px}
.btn-secondary{background:#52c41a}
.btn-sm{padding:4px 10px;font-size:12px}

.device-info{display:grid;grid-template-columns:repeat(6,1fr);gap:10px}
.device-item{background:#f8f9fa;padding:10px;border-radius:8px;text-align:center}
.device-item .label{font-size:11px;color:#666}
.device-item .value{font-size:14px;font-weight:600;color:#1a1a1a;margin-top:3px}
.online{color:#52c41a}
.offline{color:#ff4d4f}

.sms-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.sms-list{max-height:450px;overflow-y:auto}
.sms-item{padding:10px;border-bottom:1px solid #f0f0f0}
.sms-item:last-child{border-bottom:none}
.sms-item:hover{background:#f8f9fa}
.sms-header{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:6px;gap:8px}
.sms-sender{font-weight:600;font-size:13px;color:#1a1a1a}
.sms-time{font-size:11px;color:#999;white-space:nowrap}
.sms-body{font-size:13px;color:#333;line-height:1.5;word-break:break-all;background:#f8f9fa;padding:8px;border-radius:6px;margin-top:4px}
.sms-meta{display:flex;justify-content:space-between;align-items:center;margin-top:6px;gap:8px}
.sms-device{font-size:11px;color:#bbb}

.badge{font-size:11px;padding:2px 8px;border-radius:10px;white-space:nowrap}
.badge-pending{background:#fff7e6;color:#faad14}
.badge-sending{background:#e6f7ff;color:#1890ff}
.badge-ok{background:#e8f5e9;color:#52c41a}
.badge-fail{background:#ffebee;color:#ff4d4f}

.empty{text-align:center;color:#999;padding:30px;font-size:13px}
.count{font-size:12px;color:#999;font-weight:normal}

.send-form{display:flex;gap:8px;align-items:flex-end}
.send-form .form-group{flex:1;margin-bottom:0}
.send-form .form-group:first-child{flex:0 0 160px}
.login-box{max-width:400px;margin:80px auto}
.login-box input{width:100%;padding:12px;border:1px solid #e0f0f0;border-radius:8px;margin-bottom:12px;font-size:14px}

@media (max-width: 768px) {
    .sms-grid{grid-template-columns:1fr}
    .device-info{grid-template-columns:repeat(3,1fr)}
    .send-form{flex-direction:column;align-items:stretch}
    .send-form .form-group:first-child{flex:1}
}
</style>
</head>
<body>
<div class="container">

<?php if (!$isLoggedIn): ?>
<div class="card login-box">
    <div class="card-title">🔐 SMS 管理登录</div>
    <?php if (isset($error)): ?><p style="color:#ff4d4f;margin-bottom:12px"><?= $error ?></p><?php endif; ?>
    <form method="POST">
        <input type="password" name="password" placeholder="请输入管理密码" required>
        <button type="submit" name="login" class="btn" style="width:100%">登录</button>
    </form>
</div>

<?php else: ?>

<h1>📱 短信远程管理面板</h1>

<!-- 设备状态区域 -->
<div class="card">
    <div class="card-title">📊 设备状态</div>
    <div class="device-info" id="deviceWrap">
        <div class="device-item">
            <div class="label">设备ID</div>
            <div class="value dev-id"><?= htmlspecialchars($heartbeat['device_id'] ?? '未知') ?></div>
        </div>
        <div class="device-item">
            <div class="label">设备IP</div>
            <div class="value dev-ip"><?= htmlspecialchars($heartbeat['device_ip'] ?? '离线') ?></div>
        </div>
        <div class="device-item">
            <div class="label">信号强度</div>
            <div class="value dev-rssi"><?= isset($heartbeat['rssi']) ? $heartbeat['rssi'] . ' dBm' : '未知' ?></div>
        </div>
        <div class="device-item">
            <div class="label">剩余内存</div>
            <div class="value dev-heap"><?= isset($heartbeat['free_heap']) ? round($heartbeat['free_heap']/1024,1) . ' KB' : '未知' ?></div>
        </div>
        <div class="device-item">
            <div class="label">最后心跳</div>
            <div class="value dev-hb-time" style="font-size:12px"><?= htmlspecialchars($heartbeat['timestamp'] ?? '从未') ?></div>
        </div>
        <div class="device-item">
            <div class="label">在线状态</div>
            <div class="value dev-online <?= $isOnline ? 'online' : 'offline' ?>" style="font-size:14px">
                <?= $isOnline ? '🟢 在线' : '🔴 离线' ?>
            </div>
        </div>
    </div>
</div>

<div class="card">
    <div class="card-title">📤 发送短信</div>
    <?php if (isset($success)): ?><p style="color:#52c41a;margin-bottom:10px;font-size:13px">✅ <?= $success ?></p><?php endif; ?>
    <form method="POST" class="send-form">
        <input type="hidden" name="action" value="send">
        <div class="form-group">
            <label>目标号码</label>
            <input type="text" name="phone" placeholder="13800138000" required>
        </div>
        <div class="form-group">
            <label>短信内容</label>
            <input type="text" name="content" placeholder="请输入短信内容..." required>
        </div>
        <button type="submit" class="btn">📨 发送</button>
    </form>
</div>

<div class="sms-grid">
    <!-- 已接收列表 -->
    <div class="sms-col">
        <div class="card">
            <div class="card-title">
                <span>📥 已接收 <span class="count recv-count">(<?= count($receivedList) ?>)</span></span>
                <?php if (!empty($receivedList)): ?>
                    <a href="?clear=received" class="btn btn-danger btn-sm" onclick="return confirm('清空所有已接收？')">清空</a>
                <?php endif; ?>
            </div>
            <div class="sms-list" id="recvListBox">
                <?php if (empty($receivedList)): ?>
                    <div class="empty">暂无已接收短信</div>
                <?php else: ?>
                    <?php foreach ($receivedList as $sms): ?>
                    <div class="sms-item">
                        <div class="sms-header">
                            <span class="sms-sender"><?= htmlspecialchars($sms['sender'] ?? '未知') ?></span>
                            <span class="sms-time"><?= htmlspecialchars($sms['timestamp'] ?? '') ?></span>
                        </div>
                        <div class="sms-body"><?= nl2br(htmlspecialchars($sms['content'] ?? '')) ?></div>
                        <div class="sms-meta">
                            <span class="sms-device"><?= htmlspecialchars($sms['device_ip'] ?? '') ?></span>
                            <span style="font-size:11px;color:#bbb"><?= htmlspecialchars($sms['report_time'] ?? '') ?></span>
                        </div>
                    </div>
                    <?php endforeach; ?>
                <?php endif; ?>
            </div>
        </div>
    </div>

    <!-- 发送记录列表 -->
    <div class="sms-col">
        <div class="card">
            <div class="card-title">
                <span>📤 发送记录 <span class="count send-count">(<?= count($sendList) ?>)</span></span>
                <div>
                    <?php if (!empty($smsData['pending'] ?? [])): ?>
                        <a href="?clear=pending" class="btn btn-danger btn-sm" onclick="return confirm('清空待发送队列？')">清空队列</a>
                    <?php endif; ?>
                </div>
            </div>
            <div class="sms-list" id="sendListBox">
                <?php if (empty($sendList)): ?>
                    <div class="empty">暂无发送记录</div>
                <?php else: ?>
                    <?php foreach ($sendList as $sms): ?>
                    <div class="sms-item">
                        <div class="sms-header">
                            <span class="sms-sender"><?= htmlspecialchars($sms['phone'] ?? '') ?></span>
                            <span class="sms-time"><?= htmlspecialchars($sms['time'] ?? '') ?></span>
                        </div>
                        <div class="sms-body"><?= htmlspecialchars($sms['content'] ?? '') ?></div>
                        <div class="sms-meta">
                            <?php 
                            $listType = $sms['list_type'] ?? 'pending';
                            $status = $sms['status'] ?? 'pending';
                            if ($listType === 'pending') {
                                echo '<span class="badge badge-pending">⏳ 等待发送</span>';
                            } elseif ($listType === 'sending') {
                                echo '<span class="badge badge-sending">📡 发送中</span>';
                                if (!empty($sms['ack_time'])) {
                                    echo '<span style="font-size:11px;color:#999">ACK: ' . htmlspecialchars($sms['ack_time']) . '</span>';
                                }
                            } elseif ($status === 'sent') {
                                echo '<span class="badge badge-ok">✅ 发送成功</span>';
                            } elseif ($status === 'failed') {
                                echo '<span class="badge badge-fail">❌ 发送失败</span>';
                            } else {
                                echo '<span class="badge badge-ok">✅ 已发送</span>';
                            }
                            ?>
                            <?php if (!empty($sms['result_msg'])): ?>
                                <span style="font-size:11px;color:#999"><?= htmlspecialchars($sms['result_msg']) ?></span>
                            <?php endif; ?>
                        </div>
                    </div>
                    <?php endforeach; ?>
                <?php endif; ?>
            </div>
        </div>
    </div>
</div>

<p style="text-align:center;margin-top:16px">
    <a href="?logout=1" style="color:#999;text-decoration:none;font-size:13px">退出登录</a>
</p>

<?php endif; ?>

</div>

<?php if ($isLoggedIn): ?>
<script>
// 局部刷新间隔，单位 ms，可自行修改
const POLL_INTERVAL = 1000;

// HTML转义防XSS
function escapeHtml(str) {
    if (!str) return '';
    return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#039;');
}

// 更新设备状态
function updateDevice(hbData) {
    const devId = document.querySelector('.dev-id');
    const devIp = document.querySelector('.dev-ip');
    const devRssi = document.querySelector('.dev-rssi');
    const devHeap = document.querySelector('.dev-heap');
    const devHbTime = document.querySelector('.dev-hb-time');
    const devOnline = document.querySelector('.dev-online');

    devId.textContent = hbData.device_id || '未知';
    devIp.textContent = hbData.device_ip || '离线';
    devRssi.textContent = hbData.rssi ? `${hbData.rssi} dBm` : '未知';

    let heapText = '未知';
    if (typeof hbData.free_heap !== 'undefined') {
        heapText = (hbData.free_heap / 1024).toFixed(1) + ' KB';
    }
    devHeap.textContent = heapText;
    devHbTime.textContent = hbData.timestamp || '从未';

    // 判断在线：120秒内心跳即为在线
    let isOnline = false;
    let lastTime = 0;
    if (hbData.unixtime) {
        lastTime = parseInt(hbData.unixtime);
    } else if (hbData.timestamp) {
        lastTime = Math.floor(new Date(hbData.timestamp).getTime() / 1000);
    }
    const now = Math.floor(Date.now() / 1000);
    if (now - lastTime < 120) {
        isOnline = true;
    }

    devOnline.className = 'value dev-online ' + (isOnline ? 'online' : 'offline');
    devOnline.innerHTML = isOnline ? '🟢 在线' : '🔴 离线';
}

// 更新接收短信列表
function renderReceived(list) {
    const box = document.getElementById('recvListBox');
    const countEl = document.querySelector('.recv-count');
    countEl.textContent = `(${list.length})`;

    if (!list || list.length === 0) {
        box.innerHTML = '<div class="empty">暂无已接收短信</div>';
        return;
    }
    let html = '';
    list.forEach(item => {
        html += `
        <div class="sms-item">
            <div class="sms-header">
                <span class="sms-sender">${escapeHtml(item.sender || '未知')}</span>
                <span class="sms-time">${escapeHtml(item.timestamp || '')}</span>
            </div>
            <div class="sms-body">${escapeHtml(item.content || '').replace(/\n/g,'<br>')}</div>
            <div class="sms-meta">
                <span class="sms-device">${escapeHtml(item.device_ip || '')}</span>
                <span style="font-size:11px;color:#bbb">${escapeHtml(item.report_time || '')}</span>
            </div>
        </div>
        `;
    });
    box.innerHTML = html;
}

// 更新发送记录列表
function renderSendList(data) {
    const box = document.getElementById('sendListBox');
    const countEl = document.querySelector('.send-count');

    let sendList = [];
    data.pending && data.pending.forEach(s => sendList.push({...s, list_type: 'pending'}));
    data.sending && data.sending.forEach(s => sendList.push({...s, list_type: 'sending'}));
    data.sent && data.sent.forEach(s => sendList.push({...s, list_type: 'sent'}));

    // 按时间倒序
    sendList.sort((a, b) => {
        return new Date(b.time) - new Date(a.time);
    });

    countEl.textContent = `(${sendList.length})`;
    if (sendList.length === 0) {
        box.innerHTML = '<div class="empty">暂无发送记录</div>';
        return;
    }

    let html = '';
    sendList.forEach(sms => {
        let statusHtml = '';
        const listType = sms.list_type || 'pending';
        const status = sms.status || 'pending';

        if (listType === 'pending') {
            statusHtml = '<span class="badge badge-pending">⏳ 等待发送</span>';
        } else if (listType === 'sending') {
            statusHtml = '<span class="badge badge-sending">📡 发送中</span>';
            if (sms.ack_time) {
                statusHtml += `<span style="font-size:11px;color:#999">ACK: ${escapeHtml(sms.ack_time)}</span>`;
            }
        } else if (status === 'sent') {
            statusHtml = '<span class="badge badge-ok">✅ 发送成功</span>';
        } else if (status === 'failed') {
            statusHtml = '<span class="badge badge-fail">❌ 发送失败</span>';
        } else {
            statusHtml = '<span class="badge badge-ok">✅ 已发送</span>';
        }

        let resultTxt = sms.result_msg
            ? `<span style="font-size:11px;color:#999">${escapeHtml(sms.result_msg)}</span>`
            : '';

        html += `
        <div class="sms-item">
            <div class="sms-header">
                <span class="sms-sender">${escapeHtml(sms.phone || '')}</span>
                <span class="sms-time">${escapeHtml(sms.time || '')}</span>
            </div>
            <div class="sms-body">${escapeHtml(sms.content || '')}</div>
            <div class="sms-meta">
                ${statusHtml}
                ${resultTxt}
            </div>
        </div>
        `;
    });
    box.innerHTML = html;
}

// 拉取两个json文件并刷新页面
async function fetchAllData() {
    try {
        // 并发读取两个JSON文件
        const [resData, resHb] = await Promise.all([
            fetch('sms_data.json'),
            fetch('heartbeat.json')
        ]);

        const smsData = await resData.json();
        const hbData = await resHb.json();

        updateDevice(hbData);
        renderReceived(smsData.received || []);
        renderSendList(smsData);
    } catch (e) {
        // 读取失败静默忽略
        console.log('数据刷新失败', e);
    }
}

// 启动定时局部刷新
setInterval(fetchAllData, POLL_INTERVAL);
</script>
<?php endif; ?>

</body>
</html>