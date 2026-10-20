<?php
declare(strict_types=1);

// Edit these values after creating a MySQL database in Hostinger hPanel.
const DB_HOST = 'localhost';
const DB_NAME = 'YOUR_HOSTINGER_DATABASE_NAME';
const DB_USER = 'YOUR_HOSTINGER_DATABASE_USER';
const DB_PASS = 'YOUR_HOSTINGER_DATABASE_PASSWORD';

const APP_NAME = 'Savvy Mother Attendance';
const APP_TIMEZONE = 'Asia/Kolkata';
const SESSION_NAME = 'savvy_attendance_admin';

// Use the same key in the desktop app: Admin Login > Server Sync.
const SYNC_API_KEY = 'CHANGE_THIS_TO_A_LONG_RANDOM_SECRET_KEY';

// Created automatically when the dashboard tables are empty.
const INITIAL_ADMIN_USERNAME = 'savvy';
const INITIAL_ADMIN_PASSWORD = 'CHANGE_THIS_ADMIN_PASSWORD';

// ISO weekday numbers: 1=Monday ... 7=Sunday. These days are not counted absent in period reports.
const WEEKLY_OFF_DAYS = [7];
