<?php
declare(strict_types=1);

require_once __DIR__ . '/../lib.php';

header('Content-Type: application/json; charset=utf-8');

function json_response(array $payload, int $status = 200): never
{
    http_response_code($status);
    echo json_encode($payload);
    exit;
}

function incoming_api_key(): string
{
    $headers = function_exists('getallheaders') ? getallheaders() : [];
    foreach ($headers as $name => $value) {
        if (strtolower((string) $name) === 'x-api-key') {
            return trim((string) $value);
        }
    }
    if (!empty($_SERVER['HTTP_X_API_KEY'])) {
        return trim((string) $_SERVER['HTTP_X_API_KEY']);
    }
    if (!empty($_SERVER['HTTP_AUTHORIZATION']) && preg_match('/Bearer\s+(.+)/i', $_SERVER['HTTP_AUTHORIZATION'], $match)) {
        return trim($match[1]);
    }
    return '';
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    json_response(['ok' => false, 'message' => 'POST required'], 405);
}

$apiKey = incoming_api_key();
if (!SYNC_API_KEY || !hash_equals(SYNC_API_KEY, $apiKey)) {
    json_response(['ok' => false, 'message' => 'Invalid API key'], 401);
}

$rawBody = file_get_contents('php://input') ?: '';
$payload = json_decode($rawBody, true);
if (!is_array($payload)) {
    json_response(['ok' => false, 'message' => 'Invalid JSON payload'], 400);
}

$students = $payload['students'] ?? [];
$attendance = $payload['attendance'] ?? [];
if (!is_array($students) || !is_array($attendance)) {
    json_response(['ok' => false, 'message' => 'students and attendance must be arrays'], 400);
}

try {
    ensure_schema();
    $pdo = db();
    $pdo->beginTransaction();

    $studentStmt = $pdo->prepare("
        INSERT INTO students
            (student_code, name, class_name, section, roll_number, label_id, created_at)
        VALUES
            (:student_code, :name, :class_name, :section, :roll_number, :label_id, :created_at)
        ON DUPLICATE KEY UPDATE
            name = VALUES(name),
            class_name = VALUES(class_name),
            section = VALUES(section),
            roll_number = VALUES(roll_number),
            label_id = VALUES(label_id)
    ");

    $studentCount = 0;
    foreach ($students as $student) {
        if (!is_array($student)) {
            continue;
        }
        $studentCode = trim((string) ($student['student_code'] ?? ''));
        $name = trim((string) ($student['name'] ?? ''));
        $className = trim((string) ($student['class_name'] ?? ''));
        if ($studentCode === '' || $name === '' || $className === '') {
            continue;
        }
        $studentStmt->execute([
            ':student_code' => $studentCode,
            ':name' => $name,
            ':class_name' => $className,
            ':section' => trim((string) ($student['section'] ?? '')),
            ':roll_number' => trim((string) ($student['roll_number'] ?? '')),
            ':label_id' => isset($student['label_id']) && $student['label_id'] !== '' ? (int) $student['label_id'] : null,
            ':created_at' => trim((string) ($student['created_at'] ?? '')) ?: null,
        ]);
        $studentCount++;
    }

    $attendanceStmt = $pdo->prepare("
        INSERT INTO attendance
            (student_code, name, class_name, section, attendance_date, attendance_time, status, confidence)
        VALUES
            (:student_code, :name, :class_name, :section, :attendance_date, :attendance_time, :status, :confidence)
        ON DUPLICATE KEY UPDATE
            name = VALUES(name),
            class_name = VALUES(class_name),
            section = VALUES(section),
            attendance_time = VALUES(attendance_time),
            status = VALUES(status),
            confidence = VALUES(confidence)
    ");

    $attendanceCount = 0;
    foreach ($attendance as $row) {
        if (!is_array($row)) {
            continue;
        }
        $studentCode = trim((string) ($row['student_code'] ?? ''));
        $name = trim((string) ($row['name'] ?? ''));
        $className = trim((string) ($row['class_name'] ?? ''));
        $date = trim((string) ($row['date'] ?? ''));
        if ($studentCode === '' || $name === '' || $className === '' || !preg_match('/^\d{4}-\d{2}-\d{2}$/', $date)) {
            continue;
        }
        $time = trim((string) ($row['time'] ?? ''));
        if ($time !== '' && !preg_match('/^\d{2}:\d{2}(:\d{2})?$/', $time)) {
            $time = '';
        }
        $confidence = $row['confidence'] ?? null;
        $attendanceStmt->execute([
            ':student_code' => $studentCode,
            ':name' => $name,
            ':class_name' => $className,
            ':section' => trim((string) ($row['section'] ?? '')),
            ':attendance_date' => $date,
            ':attendance_time' => $time !== '' ? $time : null,
            ':status' => trim((string) ($row['status'] ?? 'Present')) ?: 'Present',
            ':confidence' => is_numeric($confidence) ? (float) $confidence : null,
        ]);
        $attendanceCount++;
    }

    $pdo->commit();
    json_response([
        'ok' => true,
        'students' => $studentCount,
        'attendance' => $attendanceCount,
    ]);
} catch (Throwable $e) {
    if (isset($pdo) && $pdo instanceof PDO && $pdo->inTransaction()) {
        $pdo->rollBack();
    }
    json_response(['ok' => false, 'message' => $e->getMessage()], 500);
}
