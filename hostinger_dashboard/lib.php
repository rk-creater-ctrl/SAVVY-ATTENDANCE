<?php
declare(strict_types=1);

require_once __DIR__ . '/config.php';

date_default_timezone_set(APP_TIMEZONE);

function db(): PDO
{
    static $pdo = null;
    if ($pdo instanceof PDO) {
        return $pdo;
    }

    $dsn = 'mysql:host=' . DB_HOST . ';dbname=' . DB_NAME . ';charset=utf8mb4';
    $pdo = new PDO($dsn, DB_USER, DB_PASS, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_EMULATE_PREPARES => false,
    ]);
    return $pdo;
}

function ensure_schema(): void
{
    $pdo = db();
    $pdo->exec("
        CREATE TABLE IF NOT EXISTS admin_users (
            id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            username VARCHAR(80) NOT NULL UNIQUE,
            password_hash VARCHAR(255) NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
    ");

    $pdo->exec("
        CREATE TABLE IF NOT EXISTS students (
            student_code VARCHAR(40) PRIMARY KEY,
            name VARCHAR(160) NOT NULL,
            class_name VARCHAR(60) NOT NULL,
            section VARCHAR(60) NOT NULL DEFAULT '',
            roll_number VARCHAR(60) NOT NULL DEFAULT '',
            label_id INT NULL,
            created_at DATETIME NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
            KEY idx_students_class (class_name, section, name)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
    ");

    $pdo->exec("
        CREATE TABLE IF NOT EXISTS attendance (
            id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            student_code VARCHAR(40) NOT NULL,
            name VARCHAR(160) NOT NULL,
            class_name VARCHAR(60) NOT NULL,
            section VARCHAR(60) NOT NULL DEFAULT '',
            attendance_date DATE NOT NULL,
            attendance_time TIME NULL,
            status VARCHAR(20) NOT NULL DEFAULT 'Present',
            confidence DECIMAL(8,3) NULL,
            synced_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
            UNIQUE KEY unique_student_day (student_code, attendance_date),
            KEY idx_attendance_date (attendance_date),
            KEY idx_attendance_class_date (class_name, section, attendance_date)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
    ");

    $count = (int) $pdo->query("SELECT COUNT(*) FROM admin_users")->fetchColumn();
    if ($count === 0) {
        $stmt = $pdo->prepare("INSERT INTO admin_users (username, password_hash) VALUES (?, ?)");
        $stmt->execute([
            INITIAL_ADMIN_USERNAME,
            password_hash(INITIAL_ADMIN_PASSWORD, PASSWORD_DEFAULT),
        ]);
    }
}

function h(?string $value): string
{
    return htmlspecialchars((string) $value, ENT_QUOTES, 'UTF-8');
}

function clean_date(?string $value): string
{
    $today = new DateTimeImmutable('today');
    if (!$value) {
        return $today->format('Y-m-d');
    }
    $date = DateTimeImmutable::createFromFormat('Y-m-d', $value);
    if (!$date) {
        return $today->format('Y-m-d');
    }
    if ($date > $today) {
        return $today->format('Y-m-d');
    }
    return $date->format('Y-m-d');
}

function period_range(string $period, string $anchorDate): array
{
    $period = in_array($period, ['today', 'week', 'month', 'year'], true) ? $period : 'today';
    $anchor = new DateTimeImmutable(clean_date($anchorDate));
    $today = new DateTimeImmutable('today');

    if ($period === 'week') {
        $start = $anchor->modify('monday this week');
        $end = $anchor->modify('sunday this week');
        $label = 'Weekly Report';
    } elseif ($period === 'month') {
        $start = $anchor->modify('first day of this month');
        $end = $anchor->modify('last day of this month');
        $label = 'Monthly Report';
    } elseif ($period === 'year') {
        $start = $anchor->setDate((int) $anchor->format('Y'), 1, 1);
        $end = $anchor->setDate((int) $anchor->format('Y'), 12, 31);
        $label = 'Yearly Report';
    } else {
        $start = $anchor;
        $end = $anchor;
        $label = 'Today Report';
    }

    if ($end > $today) {
        $end = $today;
    }
    if ($start > $end) {
        $start = $end;
    }

    return [$start->format('Y-m-d'), $end->format('Y-m-d'), $label, $period];
}

function school_days_between(string $start, string $end): array
{
    $days = [];
    $cursor = new DateTimeImmutable($start);
    $last = new DateTimeImmutable($end);
    $offDays = defined('WEEKLY_OFF_DAYS') && is_array(WEEKLY_OFF_DAYS) ? WEEKLY_OFF_DAYS : [];

    while ($cursor <= $last) {
        if (!in_array((int) $cursor->format('N'), $offDays, true)) {
            $days[] = $cursor->format('Y-m-d');
        }
        $cursor = $cursor->modify('+1 day');
    }
    return $days;
}

function fetch_students(): array
{
    $stmt = db()->query("
        SELECT student_code, name, class_name, section, roll_number
        FROM students
        ORDER BY class_name, section, name
    ");
    return $stmt->fetchAll();
}

function fetch_attendance_for_date(string $date): array
{
    $stmt = db()->prepare("
        SELECT student_code, name, class_name, section, attendance_date, attendance_time, status
        FROM attendance
        WHERE attendance_date = ? AND status = 'Present'
        ORDER BY class_name, section, name
    ");
    $stmt->execute([$date]);
    return $stmt->fetchAll();
}

function fetch_period_counts(string $start, string $end): array
{
    $stmt = db()->prepare("
        SELECT student_code, COUNT(DISTINCT attendance_date) AS present_days
        FROM attendance
        WHERE attendance_date BETWEEN ? AND ? AND status = 'Present'
        GROUP BY student_code
    ");
    $stmt->execute([$start, $end]);

    $counts = [];
    foreach ($stmt->fetchAll() as $row) {
        $counts[$row['student_code']] = (int) $row['present_days'];
    }
    return $counts;
}

function fetch_daily_counts(string $start, string $end): array
{
    $stmt = db()->prepare("
        SELECT attendance_date, COUNT(DISTINCT student_code) AS present_count
        FROM attendance
        WHERE attendance_date BETWEEN ? AND ? AND status = 'Present'
        GROUP BY attendance_date
    ");
    $stmt->execute([$start, $end]);

    $counts = [];
    foreach ($stmt->fetchAll() as $row) {
        $counts[$row['attendance_date']] = (int) $row['present_count'];
    }
    return $counts;
}

function redirect_to(string $url): never
{
    header('Location: ' . $url);
    exit;
}
