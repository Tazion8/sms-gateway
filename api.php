<?php

header('Content-Type: application/json');

$apiKey = '123456';
$dataFile = __DIR__ . '/sms_data.json';
$logFile = __DIR__ . '/sms_log.txt';
$heartbeatFile = __DIR__ . '/heartbeat.json';

// 初始化数据文件
if (!file_exists($dataFile)) {
    file_put_contents($dataFile, json_encode([
        'pending' => [],
        'sending' => [],
        'received' => [],
        'sent' => []
    ]));
}

$smsData = json_decode(file_get_contents($dataFile), true) ?: [
    'pending' => [], 'sending' => [], 'received' => [], 'sent' => []
];

// ========== 鉴权（支持 GET/POST/JSON body）==========
$receivedKey = '';

if (isset($_GET['key'])) {
    $receivedKey = $_GET['key'];
} elseif (isset($_POST['key'])) {
    $receivedKey = $_POST['key'];
} else {
    $inputRaw = file_get_contents('php://input');
    $inputJson = json_decode($inputRaw, true);
    if (isset($inputJson['key'])) {
        $receivedKey = $inputJson['key'];
    }
}

if ($receivedKey !== $apiKey) {
    http_response_code(403);
    echo json_encode(['error' => 'invalid key']);
    exit;
}

// ========== GET 请求：返回待发送短信 ==========
if ($_SERVER['REQUEST_METHOD'] === 'GET') {
    // 只返回 pending 状态的
    $toSend = array_filter($smsData['pending'] ?? [], function($s) {
        return ($s['status'] ?? 'pending') === 'pending';
    });

    $sendList = [];
    foreach ($toSend as $sms) {
        $sendList[] = [
            'id' => $sms['id'],
            'phone' => $sms['phone'],
            'content' => $sms['content']
        ];
    }

    echo json_encode([
        'send' => array_values($sendList),
        'cmd' => null
    ]);
    exit;
}

// ========== POST 请求 ==========
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $inputRaw = file_get_contents('php://input');
    $input = json_decode($inputRaw, true);

    if (!$input) {
        http_response_code(400);
        echo json_encode(['error' => 'invalid json']);
        exit;
    }

    $type = $input['type'] ?? '';

    // ===== ACK 确认 =====
    if ($type === 'ack') {
        $id = $input['id'] ?? '';
        $found = false;

        foreach ($smsData['pending'] ?? [] as &$sms) {
            if ($sms['id'] === $id) {
                $sms['status'] = 'sending';
                $sms['ack_time'] = date('Y-m-d H:i:s');
                $sms['device_id'] = $input['device_id'] ?? '';
                $found = true;
                break;
            }
        }

        if ($found) {
            file_put_contents($dataFile, json_encode($smsData, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE));
            echo json_encode(['status' => 'ok', 'type' => 'ack', 'id' => $id]);
        } else {
            echo json_encode(['status' => 'not_found', 'type' => 'ack', 'id' => $id]);
        }
        exit;
    }

    // ===== 心跳 =====
    if ($type === 'heartbeat') {
        file_put_contents($heartbeatFile, json_encode($input));
        echo json_encode(['status' => 'ok', 'type' => 'heartbeat']);
        exit;
    }

    // ===== 发送结果 =====
    if ($type === 'result') {
        $id = $input['id'] ?? '';
        $success = $input['success'] ?? false;

        $smsInfo = null;

        foreach ($smsData['pending'] ?? [] as $sms) {
            if ($sms['id'] === $id) {
                $smsInfo = $sms;
                break;
            }
        }

        if (!$smsInfo) {
            foreach ($smsData['sending'] ?? [] as $sms) {
                if ($sms['id'] === $id) {
                    $smsInfo = $sms;
                    break;
                }
            }
        }

        if ($smsInfo) {
            $smsInfo['status'] = $success ? 'sent' : 'failed';
            $smsInfo['result_time'] = date('Y-m-d H:i:s');
            $smsInfo['result_msg'] = $input['message'] ?? '';
            $smsInfo['device_id'] = $input['device_id'] ?? '';

            $smsData['sent'][] = $smsInfo;

            $smsData['pending'] = array_values(array_filter($smsData['pending'] ?? [], function($s) use ($id) {
                return $s['id'] !== $id;
            }));

            $smsData['sending'] = array_values(array_filter($smsData['sending'] ?? [], function($s) use ($id) {
                return $s['id'] !== $id;
            }));

            file_put_contents($dataFile, json_encode($smsData, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE));

            $status = $success ? '成功' : '失败';
            file_put_contents($logFile, date('Y-m-d H:i:s') . " [结果] {$id}: {$status}\n", FILE_APPEND);

            echo json_encode(['status' => 'ok', 'type' => 'result', 'id' => $id]);
        } else {
            echo json_encode(['status' => 'not_found', 'type' => 'result', 'id' => $id]);
        }
        exit;
    }

    // ===== 接收到的短信上报 =====
    if (isset($input['received']) && is_array($input['received'])) {
        foreach ($input['received'] as $sms) {
            $smsData['received'][] = [
                'sender' => $sms['sender'] ?? '',
                'content' => $sms['content'] ?? '',
                'timestamp' => $sms['timestamp'] ?? date('Y-m-d H:i:s'),
                'device_ip' => $sms['device_ip'] ?? '',
                'report_time' => date('Y-m-d H:i:s')
            ];

            file_put_contents($logFile, date('Y-m-d H:i:s') . " [接收] {$sms['sender']}: " . mb_substr($sms['content'] ?? '', 0, 30) . "\n", FILE_APPEND);
        }

        file_put_contents($dataFile, json_encode($smsData, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE));

        echo json_encode([
            'status' => 'ok',
            'type' => 'received',
            'count' => count($input['received'])
        ]);
        exit;
    }

    echo json_encode(['status' => 'ok']);
    exit;
}