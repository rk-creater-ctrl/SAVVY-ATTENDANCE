# Savvy Attendance Hostinger Dashboard

This folder is the web dashboard for `attendance.savvymotherschool.com`.

The face-recognition camera app should keep running on the school Windows PC. This dashboard runs on Hostinger and receives synced students/attendance from the desktop app.

## Upload To Hostinger

1. In Hostinger hPanel, create the subdomain `attendance.savvymotherschool.com`.
2. Create a MySQL database and database user.
3. Upload every file inside this `hostinger_dashboard` folder to the subdomain document root.
4. Edit `config.php` on the server:
   - `DB_HOST`
   - `DB_NAME`
   - `DB_USER`
   - `DB_PASS`
   - `SYNC_API_KEY`
   - `INITIAL_ADMIN_USERNAME`
   - `INITIAL_ADMIN_PASSWORD`
5. Open `https://attendance.savvymotherschool.com/`.
6. Login with the initial admin username/password from `config.php`.

The dashboard creates its tables automatically on first load.

## Connect The Desktop App

1. Open the attendance desktop app.
2. Login as admin.
3. Click `Server Sync`.
4. Use this Sync URL:

```text
https://attendance.savvymotherschool.com/api/sync.php
```

5. Paste the same `SYNC_API_KEY` from the server `config.php`.
6. Click `Sync Now`.

After setup, the desktop app syncs automatically when students are saved, attendance is marked, or attendance reports are saved.

## Reports

The dashboard shows:

- Today present/absent
- Weekly report
- Monthly report
- Yearly report
- Daily summary inside the selected period
- Present days, absent days, and attendance percentage per student

Period absent counts use `WEEKLY_OFF_DAYS` from `config.php`. By default, Sunday is not counted absent.
