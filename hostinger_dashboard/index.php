<?php
declare(strict_types=1);

require_once __DIR__ . '/lib.php';

session_name(SESSION_NAME);
session_start();

$bootError = null;
try {
    ensure_schema();
} catch (Throwable $e) {
    $bootError = $e->getMessage();
}

if (isset($_GET['logout'])) {
    $_SESSION = [];
    session_destroy();
    redirect_to('index.php');
}

$loginError = '';
$adminMessage = '';

if (!$bootError && $_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['action'] ?? '') === 'login') {
    $username = trim((string) ($_POST['username'] ?? ''));
    $password = (string) ($_POST['password'] ?? '');

    $stmt = db()->prepare("SELECT username, password_hash FROM admin_users WHERE username = ?");
    $stmt->execute([$username]);
    $admin = $stmt->fetch();

    if ($admin && password_verify($password, $admin['password_hash'])) {
        session_regenerate_id(true);
        $_SESSION['admin_username'] = $admin['username'];
        redirect_to('index.php');
    }
    $loginError = 'Invalid username or password.';
}

$isLoggedIn = !$bootError && !empty($_SESSION['admin_username']);

if ($isLoggedIn && $_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['action'] ?? '') === 'add_admin') {
    $newUsername = trim((string) ($_POST['new_username'] ?? ''));
    $newPassword = (string) ($_POST['new_password'] ?? '');
    if ($newUsername === '' || strlen($newPassword) < 8) {
        $adminMessage = 'Admin username is required and password must be at least 8 characters.';
    } else {
        try {
            $stmt = db()->prepare("INSERT INTO admin_users (username, password_hash) VALUES (?, ?)");
            $stmt->execute([$newUsername, password_hash($newPassword, PASSWORD_DEFAULT)]);
            $adminMessage = 'Admin user created.';
        } catch (Throwable $e) {
            $adminMessage = 'Could not create admin user. The username may already exist.';
        }
    }
}

function tab_url(string $period, string $date): string
{
    return 'index.php?period=' . urlencode($period) . '&date=' . urlencode($date);
}

function percent_text(int $present, int $expected): string
{
    if ($expected <= 0) {
        return '0%';
    }
    return number_format(($present / $expected) * 100, 1) . '%';
}

?><!doctype html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title><?= h(APP_NAME) ?></title>
    <link rel="stylesheet" href="assets/app.css">
</head>
<body>
<?php if ($bootError): ?>
    <main class="login-shell">
        <section class="login-panel">
            <h1><?= h(APP_NAME) ?></h1>
            <p class="error">Database/configuration error: <?= h($bootError) ?></p>
            <p class="muted">Edit <code>config.php</code> with the Hostinger MySQL database name, user, and password.</p>
        </section>
    </main>
<?php elseif (!$isLoggedIn): ?>
    <main class="login-shell">
        <form class="login-panel" method="post">
            <input type="hidden" name="action" value="login">
            <h1><?= h(APP_NAME) ?></h1>
            <p class="muted">Admin dashboard</p>
            <?php if ($loginError): ?><p class="error"><?= h($loginError) ?></p><?php endif; ?>
            <label>
                Username
                <input name="username" autocomplete="username" required>
            </label>
            <label>
                Password
                <input name="password" type="password" autocomplete="current-password" required>
            </label>
            <button type="submit">Login</button>
        </form>
    </main>
<?php else: ?>
<?php
    $anchorDate = clean_date($_GET['date'] ?? null);
    [$startDate, $endDate, $reportLabel, $period] = period_range((string) ($_GET['period'] ?? 'today'), $anchorDate);

    $students = fetch_students();
    $totalStudents = count($students);
    $presentRows = fetch_attendance_for_date($anchorDate);
    $presentCodes = array_fill_keys(array_column($presentRows, 'student_code'), true);
    $absentRows = array_values(array_filter($students, static fn(array $student): bool => !isset($presentCodes[$student['student_code']])));

    $periodCounts = fetch_period_counts($startDate, $endDate);
    $schoolDays = school_days_between($startDate, $endDate);
    $expectedDays = count($schoolDays);
    $dailyCounts = fetch_daily_counts($startDate, $endDate);

    $periodRows = [];
    $periodPresentMarks = 0;
    foreach ($students as $student) {
        $presentDays = $periodCounts[$student['student_code']] ?? 0;
        $periodPresentMarks += $presentDays;
        $periodRows[] = [
            'student' => $student,
            'present_days' => $presentDays,
            'absent_days' => max($expectedDays - $presentDays, 0),
            'percentage' => percent_text($presentDays, $expectedDays),
        ];
    }
    $periodPossibleMarks = $totalStudents * $expectedDays;
    $periodAbsentMarks = max($periodPossibleMarks - $periodPresentMarks, 0);
    $periodRate = percent_text($periodPresentMarks, $periodPossibleMarks);
?>
    <header class="topbar">
        <div>
            <h1><?= h(APP_NAME) ?></h1>
            <span><?= h($reportLabel) ?> · <?= h($startDate) ?> to <?= h($endDate) ?></span>
        </div>
        <nav>
            <span><?= h((string) $_SESSION['admin_username']) ?></span>
            <a href="index.php?logout=1">Logout</a>
        </nav>
    </header>

    <main class="page">
        <section class="toolbar">
            <div class="tabs">
                <?php foreach (['today' => 'Today', 'week' => 'Weekly', 'month' => 'Monthly', 'year' => 'Yearly'] as $key => $label): ?>
                    <a class="tab <?= $period === $key ? 'active' : '' ?>" href="<?= h(tab_url($key, $anchorDate)) ?>"><?= h($label) ?></a>
                <?php endforeach; ?>
            </div>
            <form class="date-form" method="get">
                <input type="hidden" name="period" value="<?= h($period) ?>">
                <input type="date" name="date" value="<?= h($anchorDate) ?>">
                <button type="submit">Apply</button>
            </form>
        </section>

        <section class="metrics">
            <article>
                <span>Total Students</span>
                <strong><?= $totalStudents ?></strong>
            </article>
            <article>
                <span><?= $period === 'today' ? 'Present Today' : 'Present Marks' ?></span>
                <strong><?= $period === 'today' ? count($presentRows) : $periodPresentMarks ?></strong>
            </article>
            <article>
                <span><?= $period === 'today' ? 'Absent Today' : 'Absent Marks' ?></span>
                <strong><?= $period === 'today' ? count($absentRows) : $periodAbsentMarks ?></strong>
            </article>
            <article>
                <span>Attendance Rate</span>
                <strong><?= h($periodRate) ?></strong>
            </article>
        </section>

        <?php if ($adminMessage): ?><p class="notice"><?= h($adminMessage) ?></p><?php endif; ?>

        <details class="admin-tools">
            <summary>Admin Users</summary>
            <form method="post" class="admin-form">
                <input type="hidden" name="action" value="add_admin">
                <input name="new_username" placeholder="New username" required>
                <input name="new_password" type="password" placeholder="Password, 8+ characters" required>
                <button type="submit">Create Admin</button>
            </form>
        </details>

        <section class="split">
            <div class="panel">
                <div class="panel-heading">
                    <h2>Present · <?= h($anchorDate) ?></h2>
                    <span><?= count($presentRows) ?> records</span>
                </div>
                <div class="table-wrap">
                    <table>
                        <thead><tr><th>Class</th><th>Name</th><th>Code</th><th>Time</th></tr></thead>
                        <tbody>
                        <?php foreach ($presentRows as $row): ?>
                            <tr>
                                <td><?= h(trim($row['class_name'] . ' ' . $row['section'])) ?></td>
                                <td><?= h($row['name']) ?></td>
                                <td><?= h($row['student_code']) ?></td>
                                <td><?= h((string) $row['attendance_time']) ?></td>
                            </tr>
                        <?php endforeach; ?>
                        <?php if (!$presentRows): ?><tr><td colspan="4" class="empty">No present records.</td></tr><?php endif; ?>
                        </tbody>
                    </table>
                </div>
            </div>

            <div class="panel">
                <div class="panel-heading">
                    <h2>Absent · <?= h($anchorDate) ?></h2>
                    <span><?= count($absentRows) ?> students</span>
                </div>
                <div class="table-wrap">
                    <table>
                        <thead><tr><th>Class</th><th>Name</th><th>Code</th><th>Roll</th></tr></thead>
                        <tbody>
                        <?php foreach ($absentRows as $row): ?>
                            <tr>
                                <td><?= h(trim($row['class_name'] . ' ' . $row['section'])) ?></td>
                                <td><?= h($row['name']) ?></td>
                                <td><?= h($row['student_code']) ?></td>
                                <td><?= h($row['roll_number']) ?></td>
                            </tr>
                        <?php endforeach; ?>
                        <?php if (!$absentRows): ?><tr><td colspan="4" class="empty">No absent students.</td></tr><?php endif; ?>
                        </tbody>
                    </table>
                </div>
            </div>
        </section>

        <section class="panel">
            <div class="panel-heading">
                <h2><?= h($reportLabel) ?> Register</h2>
                <span><?= $expectedDays ?> school days counted</span>
            </div>
            <div class="table-wrap">
                <table>
                    <thead>
                    <tr>
                        <th>Class</th><th>Name</th><th>Code</th><th>Present Days</th><th>Absent Days</th><th>%</th>
                    </tr>
                    </thead>
                    <tbody>
                    <?php foreach ($periodRows as $row): ?>
                        <tr>
                            <td><?= h(trim($row['student']['class_name'] . ' ' . $row['student']['section'])) ?></td>
                            <td><?= h($row['student']['name']) ?></td>
                            <td><?= h($row['student']['student_code']) ?></td>
                            <td><?= $row['present_days'] ?></td>
                            <td><?= $row['absent_days'] ?></td>
                            <td><?= h($row['percentage']) ?></td>
                        </tr>
                    <?php endforeach; ?>
                    <?php if (!$periodRows): ?><tr><td colspan="6" class="empty">No students synced yet.</td></tr><?php endif; ?>
                    </tbody>
                </table>
            </div>
        </section>

        <section class="panel">
            <div class="panel-heading">
                <h2>Daily Summary</h2>
                <span><?= h($startDate) ?> to <?= h($endDate) ?></span>
            </div>
            <div class="table-wrap compact">
                <table>
                    <thead><tr><th>Date</th><th>Present</th><th>Absent</th></tr></thead>
                    <tbody>
                    <?php foreach ($schoolDays as $day): ?>
                        <?php $presentCount = $dailyCounts[$day] ?? 0; ?>
                        <tr>
                            <td><?= h($day) ?></td>
                            <td><?= $presentCount ?></td>
                            <td><?= max($totalStudents - $presentCount, 0) ?></td>
                        </tr>
                    <?php endforeach; ?>
                    <?php if (!$schoolDays): ?><tr><td colspan="3" class="empty">No school days in this range.</td></tr><?php endif; ?>
                    </tbody>
                </table>
            </div>
        </section>
    </main>
<?php endif; ?>
</body>
</html>
