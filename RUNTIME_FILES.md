# Runtime Files

Keep these files beside the built `SavvyAttendanceCpp.exe`:

- `trainer.yml`
- `labels.txt`
- `attendance.db`
- `haarcascade_frontalface_default.xml`

Optional:

- `school_logo.webp`

This C++ app is intentionally attendance-focused.

It does not require `dataset/` for normal attendance startup and recognition when you already have:

- `trainer.yml`
- `labels.txt`

If you retrain on another machine, replace these runtime files on the target machine:

- `trainer.yml`
- `trainer_meta.json`
- `labels.txt`
- optionally `attendance.db` if you also want synced database changes
