#define main savvy_legacy_opencv_main
#include "main.cpp"
#undef main

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QIcon>
#include <QIODevice>
#include <QList>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QScrollArea>
#include <QSet>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

constexpr double kUnknownFaceMatchThreshold = 65.0;

QString qstr(const std::string& value) {
    return QString::fromStdString(value);
}

std::string str(const QString& value) {
    return value.trimmed().toStdString();
}

QStringList classOrder() {
    return {"Nursery", "LKG", "UKG", "1st", "2nd", "3rd", "4th", "5th",
            "6th", "7th", "8th", "9th", "10th", "11th", "12th", "Staff"};
}

QString resolveRuntimeDataRoot() {
    QString publicDir = QString::fromLocal8Bit(qgetenv("PUBLIC"));
    if (publicDir.trimmed().isEmpty()) {
        publicDir = "C:/Users/Public";
    }
    return QDir::cleanPath(QDir(publicDir).filePath("Documents/SavvyAttendanceSystem"));
}

QString readTextFileQt(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("Could not read file: " + path.toStdString());
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

QString normalizeServerUrl(QString url) {
    url = url.trimmed();
    if (url.isEmpty()) {
        return "";
    }
    if (!url.startsWith("http://", Qt::CaseInsensitive) && !url.startsWith("https://", Qt::CaseInsensitive)) {
        url = "https://" + url;
    }
    const QUrl parsed(url);
    if (parsed.path().isEmpty() || parsed.path() == "/") {
        url = url.trimmed().remove(QRegularExpression("/+$")) + "/api/sync.php";
    }
    return url;
}

QString normalizeCountryCode(QString code) {
    code.remove(QRegularExpression("\\D"));
    return code.isEmpty() ? "91" : code;
}

QString normalizeWhatsAppNumber(QString raw, const QString& defaultCountryCode) {
    raw = raw.trimmed();
    if (raw.isEmpty()) {
        return "";
    }
    const bool hasCountryPrefix = raw.startsWith("+") || raw.startsWith("00");
    QString digits = raw;
    digits.remove(QRegularExpression("\\D"));
    if (digits.startsWith("00")) {
        digits = digits.mid(2);
    }
    if (digits.isEmpty()) {
        return "";
    }
    if (hasCountryPrefix) {
        return digits.length() >= 8 && digits.length() <= 15 ? digits : "";
    }
    if (digits.length() == 10) {
        return defaultCountryCode + digits;
    }
    if (digits.length() == 11 && digits.startsWith("0")) {
        return defaultCountryCode + digits.mid(1);
    }
    if (digits.length() >= 11 && digits.length() <= 15) {
        return digits;
    }
    return "";
}

QStringList parseWhatsAppNumbers(const QString& numbersText, const QString& countryCode, QStringList* invalidOut = nullptr) {
    QStringList numbers;
    QSet<QString> seen;
    const auto parts = numbersText.split(QRegularExpression("[\\n,;]+"), Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        const QString normalized = normalizeWhatsAppNumber(part, countryCode);
        if (normalized.isEmpty()) {
            if (invalidOut != nullptr) {
                invalidOut->append(part.trimmed());
            }
            continue;
        }
        if (!seen.contains(normalized)) {
            seen.insert(normalized);
            numbers.append(normalized);
        }
    }
    return numbers;
}

QJsonObject readJsonObject(const std::filesystem::path& path) {
    QFile file(qstr(path.string()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void writeJsonObject(const std::filesystem::path& path, const QJsonObject& object) {
    QFile file(qstr(path.string()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("Could not write settings: " + path.string());
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

void ensureStudentTable(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS school_students ("
        "label_id INTEGER PRIMARY KEY,"
        "student_code TEXT UNIQUE,"
        "name TEXT,"
        "class_name TEXT,"
        "section TEXT DEFAULT '',"
        "roll_number TEXT DEFAULT '',"
        "created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");";
    char* errorMessage = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
        std::string error = errorMessage ? errorMessage : "unknown error";
        sqlite3_free(errorMessage);
        throw std::runtime_error("Could not initialize student table: " + error);
    }
    sqlite3_exec(db, "ALTER TABLE school_students ADD COLUMN created_at TEXT DEFAULT '';", nullptr, nullptr, nullptr);
}

int nextLabelIdFromFile() {
    int nextId = 1;
    try {
        for (const auto& [label, student] : loadStudents()) {
            (void)student;
            nextId = std::max(nextId, label + 1);
        }
    } catch (...) {
    }
    return nextId;
}

std::string safeFolderText(std::string value) {
    for (char& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') {
            ch = '_';
        }
    }
    return value.empty() ? "student" : value;
}

void appendStudentLabel(const Student& student) {
    std::ofstream out(appOutputPath(kLabelsFile), std::ios::app);
    if (!out) {
        throw std::runtime_error("Could not update labels.txt");
    }
    out << student.labelId << ',' << student.name << ',' << student.studentCode << ',' << student.className << '\n';
}

void saveStudentRecord(sqlite3* db, const Student& student) {
    ensureStudentTable(db);
    const char* sql =
        "INSERT OR REPLACE INTO school_students "
        "(label_id, student_code, name, class_name, section, roll_number) VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Could not prepare student save.");
    }
    sqlite3_bind_int(stmt, 1, student.labelId);
    sqlite3_bind_text(stmt, 2, student.studentCode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, student.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, student.className.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, student.section.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, student.rollNumber.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) {
        throw std::runtime_error("Could not save student to database.");
    }
}

cv::VideoCapture openCameraIndex(int index) {
    cv::VideoCapture cap(index, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        cap.open(index);
    }
    if (!cap.isOpened()) {
        throw std::runtime_error("Could not open camera index " + std::to_string(index));
    }
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 15);
    return cap;
}

QImage matToImage(const cv::Mat& frame) {
    cv::Mat rgb;
    cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
}

class AttendanceWindow : public QMainWindow {
public:
    AttendanceWindow() {
        initializeRuntimePaths(QCoreApplication::applicationFilePath().toLocal8Bit().constData());
        setRuntimeDataRoot(resolveRuntimeDataRoot().toStdString());
        setWindowTitle("Savvy Attendance System");
        const auto iconPath = findApplicationAssetFile("logo.png");
        if (!iconPath.empty()) {
            setWindowIcon(QIcon(qstr(iconPath.string())));
        }
        resize(1366, 820);
        setMinimumSize(1100, 720);
        setStyleSheet(
            "QMainWindow, QWidget { background:#07111f; color:#e5edf7; font-family:Segoe UI; font-size:14px; }"
            "QPushButton { background:#1d4ed8; color:white; border:0; border-radius:12px; padding:14px 18px; font-weight:700; min-height:24px; }"
            "QPushButton:hover { background:#2563eb; }"
            "QPushButton:disabled { background:#334155; color:#94a3b8; }"
            "QLineEdit, QComboBox, QSpinBox, QTextEdit, QPlainTextEdit { background:#0f172a; color:#e5edf7; border:1px solid #334155; border-radius:10px; padding:10px; selection-background-color:#2563eb; }"
            "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QTextEdit:focus, QPlainTextEdit:focus { border:1px solid #38bdf8; }"
            "QGroupBox { background:#0b1628; border:1px solid #26364f; border-radius:18px; margin-top:16px; padding:18px; font-weight:800; font-size:15px; }"
            "QGroupBox::title { subcontrol-origin:margin; left:18px; padding:0 8px; color:#bfdbfe; }"
            "QTabWidget::pane { border:0; padding-top:10px; }"
            "QTabBar::tab { background:#0f172a; color:#cbd5e1; padding:13px 20px; margin-right:4px; border-top-left-radius:10px; border-top-right-radius:10px; font-weight:700; }"
            "QTabBar::tab:selected { background:#1d4ed8; color:white; }"
            "QCheckBox { spacing:8px; padding:3px; }"
            "QScrollArea { border:0; background:#07111f; }");

        tabs = new QTabWidget(this);
        setCentralWidget(tabs);
        buildDashboardTab();
        buildRegisterTab();
        buildAttendanceTab();
        buildReportsTab();
        buildSettingsTab();

        loadSettings();
        writeStartupDiagnostics();
        log("Runtime data root: " + qstr(cleanPath(gDataDir).string()));
        log("Dataset path: " + qstr(datasetPath().string()) +
            (std::filesystem::is_directory(datasetPath()) ? " (exists)" : " (missing)"));
        log("Labels path: " + qstr(labelsPath().string()) +
            " (" + QString::number(static_cast<qulonglong>(safeFileSize(labelsPath()))) + " bytes)");
        log("Supported dataset images found: " + QString::number(countSupportedDatasetImages()));
        startCoreLoad();
    }

    ~AttendanceWindow() override {
        stopCamera();
        if (db != nullptr) {
            try {
                saveDailyReports(db, state.students);
            } catch (...) {
            }
            sqlite3_close(db);
        }
    }

private:
    QTabWidget* tabs = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* modelLabel = nullptr;
    QLabel* videoLabel = nullptr;
    QLabel* previewLabel = nullptr;
    QLineEdit* nameEdit = nullptr;
    QLineEdit* codeEdit = nullptr;
    QComboBox* classEdit = nullptr;
    QLineEdit* sectionEdit = nullptr;
    QLineEdit* rollEdit = nullptr;
    QSpinBox* cameraIndexSpin = nullptr;
    QLineEdit* studentArrivalEdit = nullptr;
    QLineEdit* staffArrivalEdit = nullptr;
    QLineEdit* autosaveEdit = nullptr;
    QLineEdit* whatsappMessageFileEdit = nullptr;
    QLineEdit* whatsappCountryEdit = nullptr;
    QPlainTextEdit* whatsappNumbersEdit = nullptr;
    QCheckBox* whatsappAutoAfterAttendanceCheck = nullptr;
    QCheckBox* whatsappAutoPressSendCheck = nullptr;
    QCheckBox* whatsappManualWithAutoCheck = nullptr;
    std::map<QString, QLineEdit*> whatsappGroupEdits;
    std::map<QString, QCheckBox*> whatsappManualChecks;
    QLineEdit* serverUrlEdit = nullptr;
    QLineEdit* serverApiKeyEdit = nullptr;
    QTextEdit* logBox = nullptr;
    QPushButton* startAttendanceButton = nullptr;
    QPushButton* trainButton = nullptr;
    QPushButton* registerButton = nullptr;
    QPushButton* dashboardReportButton = nullptr;
    QPushButton* dashboardPreviewButton = nullptr;
    QPushButton* registerCaptureButton = nullptr;
    QPushButton* attendanceStartButton = nullptr;
    QPushButton* attendancePreviewButton = nullptr;
    QPushButton* stopCameraButton = nullptr;
    QList<QPushButton*> guardedButtons;

    AppState state;
    sqlite3* db = nullptr;
    ArrivalSettings arrivalSettings;
    cv::VideoCapture camera;
    QTimer cameraTimer;
    QTimer previewTimer;
    RecognizedCandidate candidate;
    std::map<std::string, std::chrono::steady_clock::time_point> lastRecognized;
    std::unordered_set<std::string> markedTodayCodes;
    std::string currentAttendanceDate;
    std::chrono::steady_clock::time_point lastUnknownSavedTime{};
    int unknownCandidateCount = 0;
    bool attendanceMode = false;
    cv::Ptr<cv::face::LBPHFaceRecognizer> unknownRecognizer;
    bool unknownFacesLoaded = false;
    int unknownFaceCount = 0;
    bool processBusy = false;
    QString activeProcessName;

    void log(const QString& text) {
        if (logBox != nullptr) {
            logBox->append(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + text);
        }
        if (statusLabel != nullptr) {
            statusLabel->setText(text);
        }
    }

    void ensureDb() {
        if (db == nullptr) {
            db = openDatabase();
            initAttendanceTable(db);
            ensureStudentTable(db);
        }
    }

    void refreshMarkedToday() {
        markedTodayCodes.clear();
        currentAttendanceDate = nowDate();
        ensureDb();
        const auto rows = loadAttendanceRows(db, currentAttendanceDate);
        for (const auto& row : rows) {
            markedTodayCodes.insert(row.studentCode);
        }
    }

    void addGuardedButton(QPushButton* button) {
        if (button != nullptr && !guardedButtons.contains(button)) {
            guardedButtons.append(button);
        }
    }

    void updateActionButtons() {
        for (auto* button : guardedButtons) {
            if (button != nullptr) {
                button->setEnabled(!processBusy);
            }
        }
        if (!processBusy) {
            if (startAttendanceButton != nullptr) startAttendanceButton->setEnabled(state.ready);
            if (attendanceStartButton != nullptr) attendanceStartButton->setEnabled(state.ready);
            if (stopCameraButton != nullptr) stopCameraButton->setEnabled(camera.isOpened());
        } else if (stopCameraButton != nullptr) {
            stopCameraButton->setEnabled(camera.isOpened());
        }
    }

    bool beginProcess(const QString& name, bool allowStopCamera = false) {
        if (processBusy) {
            QMessageBox::information(this, "Please Wait",
                                     activeProcessName + " is already running.\n\nPlease wait until it finishes.");
            return false;
        }
        processBusy = true;
        activeProcessName = name;
        for (auto* button : guardedButtons) {
            if (button != nullptr) {
                button->setEnabled(false);
            }
        }
        if (stopCameraButton != nullptr) {
            stopCameraButton->setEnabled(allowStopCamera && camera.isOpened());
        }
        log(name + " running...");
        return true;
    }

    void endProcess(const QString& message = QString()) {
        processBusy = false;
        activeProcessName.clear();
        updateActionButtons();
        if (!message.isEmpty()) {
            log(message);
        }
    }

    void buildDashboardTab() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(34, 28, 34, 28);
        layout->setSpacing(18);

        auto* title = new QLabel("Smart Attendance System");
        title->setStyleSheet("font-size:34px; font-weight:900; color:#f8fafc;");
        auto* subtitle = new QLabel("Production dashboard for registration, training, attendance, reports, WhatsApp and server sync.");
        subtitle->setStyleSheet("font-size:15px; color:#94a3b8;");
        statusLabel = new QLabel("Starting...");
        statusLabel->setStyleSheet("font-size:17px; color:#7dd3fc; font-weight:700;");
        modelLabel = new QLabel("Face model: loading");
        modelLabel->setStyleSheet("color:#cbd5e1;");

        auto* actions = new QGroupBox("Main Actions");
        auto* actionLayout = new QGridLayout(actions);
        actionLayout->setSpacing(16);
        startAttendanceButton = new QPushButton("Start Attendance");
        trainButton = new QPushButton("Train Model");
        registerButton = new QPushButton("Register Student");
        dashboardReportButton = new QPushButton("Save Reports");
        dashboardPreviewButton = new QPushButton("Camera Preview");
        startAttendanceButton->setStyleSheet("background:#16a34a; font-size:16px; min-height:58px;");
        registerButton->setStyleSheet("background:#2563eb; font-size:16px; min-height:58px;");
        trainButton->setStyleSheet("background:#ca8a04; font-size:16px; min-height:58px;");
        dashboardReportButton->setStyleSheet("background:#475569; font-size:16px; min-height:58px;");
        dashboardPreviewButton->setStyleSheet("background:#0891b2; font-size:16px; min-height:58px;");
        actionLayout->addWidget(startAttendanceButton, 0, 0);
        actionLayout->addWidget(registerButton, 0, 1);
        actionLayout->addWidget(trainButton, 0, 2);
        actionLayout->addWidget(dashboardReportButton, 1, 0);
        actionLayout->addWidget(dashboardPreviewButton, 1, 1);
        actionLayout->setColumnStretch(0, 1);
        actionLayout->setColumnStretch(1, 1);
        actionLayout->setColumnStretch(2, 1);

        logBox = new QTextEdit;
        logBox->setReadOnly(true);
        logBox->setMinimumHeight(300);
        logBox->setStyleSheet("background:#020617; border:1px solid #1e293b; border-radius:16px; padding:12px;");

        layout->addWidget(title);
        layout->addWidget(subtitle);
        layout->addWidget(statusLabel);
        layout->addWidget(modelLabel);
        layout->addWidget(actions);
        layout->addWidget(logBox, 1);

        connect(startAttendanceButton, &QPushButton::clicked, this, [this] {
            tabs->setCurrentIndex(2);
            startAttendance();
        });
        connect(registerButton, &QPushButton::clicked, this, [this] { tabs->setCurrentIndex(1); });
        connect(trainButton, &QPushButton::clicked, this, [this] { trainModelAsync(); });
        connect(dashboardReportButton, &QPushButton::clicked, this, [this] { saveReports(); });
        connect(dashboardPreviewButton, &QPushButton::clicked, this, [this] { tabs->setCurrentIndex(2); startPreview(); });
        addGuardedButton(startAttendanceButton);
        addGuardedButton(registerButton);
        addGuardedButton(trainButton);
        addGuardedButton(dashboardReportButton);
        addGuardedButton(dashboardPreviewButton);

        tabs->addTab(page, "Dashboard");
    }

    void buildRegisterTab() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(34, 28, 34, 28);
        layout->setSpacing(18);
        auto* title = new QLabel("Register Student");
        title->setStyleSheet("font-size:28px; font-weight:900; color:#f8fafc;");
        auto* help = new QLabel("Fill student details, then capture face images using the selected camera index.");
        help->setStyleSheet("color:#94a3b8;");
        auto* formBox = new QGroupBox("Register Student");
        auto* form = new QFormLayout(formBox);
        form->setLabelAlignment(Qt::AlignRight);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(14);
        nameEdit = new QLineEdit;
        codeEdit = new QLineEdit;
        classEdit = new QComboBox;
        classEdit->addItems({"Nursery", "LKG", "UKG", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "9th", "10th", "11th", "12th", "Staff"});
        sectionEdit = new QLineEdit;
        rollEdit = new QLineEdit;
        form->addRow("Name", nameEdit);
        form->addRow("Student Code", codeEdit);
        form->addRow("Class", classEdit);
        form->addRow("Section", sectionEdit);
        form->addRow("Roll Number", rollEdit);

        registerCaptureButton = new QPushButton("Save Student And Capture Faces");
        registerCaptureButton->setStyleSheet("background:#16a34a; font-size:16px; min-height:54px;");
        layout->addWidget(title);
        layout->addWidget(help);
        layout->addWidget(formBox);
        layout->addWidget(registerCaptureButton);
        layout->addStretch();
        connect(registerCaptureButton, &QPushButton::clicked, this, [this] { registerStudent(); });
        addGuardedButton(registerCaptureButton);
        tabs->addTab(page, "Register");
    }

    void buildAttendanceTab() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(26, 22, 26, 22);
        layout->setSpacing(14);
        videoLabel = new QLabel("Camera preview will appear here");
        videoLabel->setMinimumSize(900, 560);
        videoLabel->setAlignment(Qt::AlignCenter);
        videoLabel->setStyleSheet("background:#020617; border:1px solid #334155; border-radius:16px;");
        auto* buttons = new QHBoxLayout;
        attendanceStartButton = new QPushButton("Start Attendance");
        attendancePreviewButton = new QPushButton("Preview Only");
        stopCameraButton = new QPushButton("Stop Camera");
        auto* home = new QPushButton("Back Dashboard");
        attendanceStartButton->setStyleSheet("background:#16a34a;");
        attendancePreviewButton->setStyleSheet("background:#0891b2;");
        stopCameraButton->setStyleSheet("background:#b91c1c;");
        home->setStyleSheet("background:#475569;");
        buttons->addWidget(attendanceStartButton);
        buttons->addWidget(attendancePreviewButton);
        buttons->addWidget(stopCameraButton);
        buttons->addWidget(home);
        layout->addWidget(videoLabel, 1);
        layout->addLayout(buttons);
        connect(attendanceStartButton, &QPushButton::clicked, this, [this] { startAttendance(); });
        connect(attendancePreviewButton, &QPushButton::clicked, this, [this] { startPreview(); });
        connect(stopCameraButton, &QPushButton::clicked, this, [this] { stopCamera(); });
        connect(home, &QPushButton::clicked, this, [this] { tabs->setCurrentIndex(0); });
        connect(&cameraTimer, &QTimer::timeout, this, [this] { processAttendanceFrame(); });
        connect(&previewTimer, &QTimer::timeout, this, [this] { processPreviewFrame(); });
        addGuardedButton(attendanceStartButton);
        addGuardedButton(attendancePreviewButton);
        addGuardedButton(home);
        tabs->addTab(page, "Attendance");
    }

    void buildReportsTab() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(34, 28, 34, 28);
        layout->setSpacing(18);

        auto* title = new QLabel("Reports & Integrations");
        title->setStyleSheet("font-size:28px; font-weight:900; color:#f8fafc;");
        auto* help = new QLabel("Manage present/absent reports, WhatsApp messages, class groups, and server sync.");
        help->setStyleSheet("color:#94a3b8;");
        layout->addWidget(title);
        layout->addWidget(help);

        auto* reportBox = new QGroupBox("Reports");
        auto* reportLayout = new QHBoxLayout(reportBox);
        auto* save = new QPushButton("Generate Present / Absent Reports");
        auto* unknown = new QPushButton("Open Unknown Faces Folder");
        save->setStyleSheet("background:#475569;");
        unknown->setStyleSheet("background:#0891b2;");
        reportLayout->addWidget(save);
        reportLayout->addWidget(unknown);

        auto* whatsappBox = new QGroupBox("WhatsApp Message");
        auto* whatsappLayout = new QVBoxLayout(whatsappBox);
        auto* whatsappForm = new QFormLayout;
        whatsappMessageFileEdit = new QLineEdit;
        auto* browseMessage = new QPushButton("Browse Message .txt");
        auto* fileRow = new QHBoxLayout;
        fileRow->addWidget(whatsappMessageFileEdit, 1);
        fileRow->addWidget(browseMessage);
        whatsappCountryEdit = new QLineEdit("91");
        whatsappNumbersEdit = new QPlainTextEdit;
        whatsappNumbersEdit->setPlaceholderText("Office/mobile numbers separated by comma, semicolon, or new line");
        whatsappNumbersEdit->setMaximumHeight(90);
        whatsappAutoAfterAttendanceCheck = new QCheckBox("Start WhatsApp automatically after Attendance Completed");
        whatsappAutoPressSendCheck = new QCheckBox("Press Enter automatically to send each WhatsApp message");
        whatsappManualWithAutoCheck = new QCheckBox("Send manual .txt to ticked class groups after automatic attendance reports");
        whatsappForm->addRow("Manual Message .txt", fileRow);
        whatsappForm->addRow("Country Code", whatsappCountryEdit);
        whatsappForm->addRow("Office Numbers", whatsappNumbersEdit);
        whatsappLayout->addLayout(whatsappForm);
        whatsappLayout->addWidget(whatsappAutoAfterAttendanceCheck);
        whatsappLayout->addWidget(whatsappAutoPressSendCheck);
        whatsappLayout->addWidget(whatsappManualWithAutoCheck);

        auto* classGroupBox = new QGroupBox("Class Group Names");
        auto* classGrid = new QGridLayout(classGroupBox);
        int index = 0;
        for (const QString& className : classOrder()) {
            auto* manualCheck = new QCheckBox(className);
            auto* groupEdit = new QLineEdit;
            groupEdit->setPlaceholderText(className + " WhatsApp group name");
            whatsappManualChecks[className] = manualCheck;
            whatsappGroupEdits[className] = groupEdit;
            const int row = index / 2;
            const int col = (index % 2) * 2;
            classGrid->addWidget(manualCheck, row, col);
            classGrid->addWidget(groupEdit, row, col + 1);
            ++index;
        }
        whatsappLayout->addWidget(classGroupBox);

        auto* whatsappButtons = new QHBoxLayout;
        auto* saveWhatsApp = new QPushButton("Save WhatsApp Settings");
        auto* openNextNumber = new QPushButton("Open Numbers");
        auto* sendManualGroups = new QPushButton("Send Manual Groups");
        auto* openWhatsApp = new QPushButton("Open WhatsApp Web");
        saveWhatsApp->setStyleSheet("background:#2563eb;");
        openNextNumber->setStyleSheet("background:#059669;");
        sendManualGroups->setStyleSheet("background:#0d9488;");
        openWhatsApp->setStyleSheet("background:#475569;");
        whatsappButtons->addWidget(saveWhatsApp);
        whatsappButtons->addWidget(openNextNumber);
        whatsappButtons->addWidget(sendManualGroups);
        whatsappButtons->addWidget(openWhatsApp);
        whatsappLayout->addLayout(whatsappButtons);

        auto* serverBox = new QGroupBox("Server Sync");
        auto* serverLayout = new QVBoxLayout(serverBox);
        auto* serverForm = new QFormLayout;
        serverUrlEdit = new QLineEdit;
        serverApiKeyEdit = new QLineEdit;
        serverApiKeyEdit->setEchoMode(QLineEdit::Password);
        serverForm->addRow("Sync URL", serverUrlEdit);
        serverForm->addRow("API Key", serverApiKeyEdit);
        auto* serverButtons = new QHBoxLayout;
        auto* saveServer = new QPushButton("Save Server Settings");
        auto* syncNow = new QPushButton("Sync Now");
        auto* openDashboard = new QPushButton("Open Dashboard");
        saveServer->setStyleSheet("background:#2563eb;");
        syncNow->setStyleSheet("background:#16a34a;");
        openDashboard->setStyleSheet("background:#475569;");
        serverButtons->addWidget(saveServer);
        serverButtons->addWidget(syncNow);
        serverButtons->addWidget(openDashboard);
        serverLayout->addLayout(serverForm);
        serverLayout->addLayout(serverButtons);

        layout->addWidget(reportBox);
        layout->addWidget(whatsappBox);
        layout->addWidget(serverBox);
        connect(save, &QPushButton::clicked, this, [this] { saveReports(); });
        connect(unknown, &QPushButton::clicked, this, [] {
            std::filesystem::create_directories(appOutputPath(kUnknownFacesPath));
            QDesktopServices::openUrl(QUrl::fromLocalFile(qstr(appOutputPath(kUnknownFacesPath).string())));
        });
        connect(browseMessage, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(this, "Choose WhatsApp message text file", QString(), "Text files (*.txt);;All files (*.*)");
            if (!file.isEmpty()) {
                whatsappMessageFileEdit->setText(file);
            }
        });
        connect(saveWhatsApp, &QPushButton::clicked, this, [this] { saveWhatsAppSettings(); });
        connect(openNextNumber, &QPushButton::clicked, this, [this] { openWhatsAppNumbers(); });
        connect(sendManualGroups, &QPushButton::clicked, this, [this] { sendManualWhatsAppGroups(); });
        connect(openWhatsApp, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl("https://web.whatsapp.com/"));
        });
        connect(saveServer, &QPushButton::clicked, this, [this] { saveServerSettings(); });
        connect(syncNow, &QPushButton::clicked, this, [this] { syncServerNow(); });
        connect(openDashboard, &QPushButton::clicked, this, [this] { openServerDashboard(); });
        addGuardedButton(save);
        addGuardedButton(unknown);
        addGuardedButton(browseMessage);
        addGuardedButton(openNextNumber);
        addGuardedButton(sendManualGroups);
        addGuardedButton(openWhatsApp);
        addGuardedButton(syncNow);
        addGuardedButton(openDashboard);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        tabs->addTab(scroll, "Reports & Integrations");
    }

    void buildSettingsTab() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(34, 28, 34, 28);
        layout->setSpacing(18);
        auto* title = new QLabel("System Settings");
        title->setStyleSheet("font-size:28px; font-weight:900; color:#f8fafc;");
        auto* help = new QLabel("Set camera index, late arrival times, and autosave folder.");
        help->setStyleSheet("color:#94a3b8;");
        auto* formBox = new QGroupBox("Settings");
        auto* form = new QFormLayout(formBox);
        form->setLabelAlignment(Qt::AlignRight);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(14);
        cameraIndexSpin = new QSpinBox;
        cameraIndexSpin->setRange(0, 9);
        cameraIndexSpin->setValue(0);
        studentArrivalEdit = new QLineEdit;
        staffArrivalEdit = new QLineEdit;
        autosaveEdit = new QLineEdit;
        auto* browse = new QPushButton("Browse Autosave Folder");
        form->addRow("Camera Index", cameraIndexSpin);
        form->addRow("Student Late After HH:MM", studentArrivalEdit);
        form->addRow("Staff Late After HH:MM", staffArrivalEdit);
        form->addRow("Autosave Folder", autosaveEdit);
        form->addRow("", browse);
        auto* save = new QPushButton("Save Settings");
        browse->setStyleSheet("background:#475569;");
        save->setStyleSheet("background:#16a34a; font-size:16px; min-height:54px;");
        layout->addWidget(title);
        layout->addWidget(help);
        layout->addWidget(formBox);
        layout->addWidget(save);
        layout->addStretch();
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString folder = QFileDialog::getExistingDirectory(this, "Select Autosave Folder");
            if (!folder.isEmpty()) {
                autosaveEdit->setText(folder);
            }
        });
        connect(save, &QPushButton::clicked, this, [this] { saveSettings(); });
        addGuardedButton(browse);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        tabs->addTab(scroll, "Settings");
    }

    void loadSettings() {
        QSettings settings(appOutputPath("savvy_qt_settings.ini").string().c_str(), QSettings::IniFormat);
        if (cameraIndexSpin) cameraIndexSpin->setValue(settings.value("camera/index", 0).toInt());
        if (autosaveEdit) autosaveEdit->setText(settings.value("autosave/folder", "").toString());
        const auto arrival = loadArrivalSettings();
        if (studentArrivalEdit) studentArrivalEdit->setText(qstr(arrival.studentArrivalTime));
        if (staffArrivalEdit) staffArrivalEdit->setText(qstr(arrival.staffArrivalTime));
        loadWhatsAppSettings();
        loadServerSettings();
    }

    void saveSettings() {
        QSettings settings(appOutputPath("savvy_qt_settings.ini").string().c_str(), QSettings::IniFormat);
        settings.setValue("camera/index", cameraIndexSpin->value());
        settings.setValue("autosave/folder", autosaveEdit->text());
        std::ofstream arrival(appOutputPath(kArrivalSettingsFile));
        arrival << "{\n"
                << "  \"student_arrival_time\": \"" << str(studentArrivalEdit->text()) << "\",\n"
                << "  \"staff_arrival_time\": \"" << str(staffArrivalEdit->text()) << "\"\n"
                << "}\n";
        log("Settings saved.");
    }

    void loadWhatsAppSettings() {
        const QJsonObject settings = readJsonObject(appOutputPath("whatsapp_settings.json"));
        if (whatsappMessageFileEdit) whatsappMessageFileEdit->setText(settings.value("message_file").toString());
        if (whatsappCountryEdit) whatsappCountryEdit->setText(settings.value("country_code").toString("91"));
        if (whatsappNumbersEdit) whatsappNumbersEdit->setPlainText(settings.value("numbers").toString());
        if (whatsappAutoAfterAttendanceCheck) whatsappAutoAfterAttendanceCheck->setChecked(settings.value("auto_after_attendance").toBool(true));
        if (whatsappAutoPressSendCheck) whatsappAutoPressSendCheck->setChecked(settings.value("auto_press_send").toBool(true));
        if (whatsappManualWithAutoCheck) whatsappManualWithAutoCheck->setChecked(settings.value("send_manual_with_auto").toBool(false));

        const QJsonObject groups = settings.value("class_groups").toObject();
        const QJsonArray manualClasses = settings.value("manual_group_classes").toArray();
        QSet<QString> manualSet;
        for (const auto& value : manualClasses) {
            manualSet.insert(value.toString());
        }
        for (const QString& className : classOrder()) {
            if (whatsappGroupEdits.count(className)) {
                whatsappGroupEdits[className]->setText(groups.value(className).toString());
            }
            if (whatsappManualChecks.count(className)) {
                whatsappManualChecks[className]->setChecked(manualSet.contains(className));
            }
        }
    }

    QJsonObject collectWhatsAppSettings() const {
        QJsonObject groups;
        QJsonArray manualClasses;
        for (const QString& className : classOrder()) {
            const auto groupIt = whatsappGroupEdits.find(className);
            const auto checkIt = whatsappManualChecks.find(className);
            groups[className] = groupIt != whatsappGroupEdits.end() ? groupIt->second->text().trimmed() : "";
            if (checkIt != whatsappManualChecks.end() && checkIt->second->isChecked()) {
                manualClasses.append(className);
            }
        }
        QJsonObject settings;
        settings["message_file"] = whatsappMessageFileEdit ? whatsappMessageFileEdit->text().trimmed() : "";
        settings["country_code"] = whatsappCountryEdit ? normalizeCountryCode(whatsappCountryEdit->text()) : "91";
        settings["numbers"] = whatsappNumbersEdit ? whatsappNumbersEdit->toPlainText().trimmed() : "";
        settings["class_groups"] = groups;
        settings["manual_group_classes"] = manualClasses;
        settings["send_manual_with_auto"] = whatsappManualWithAutoCheck && whatsappManualWithAutoCheck->isChecked();
        settings["auto_after_attendance"] = !whatsappAutoAfterAttendanceCheck || whatsappAutoAfterAttendanceCheck->isChecked();
        settings["auto_press_send"] = !whatsappAutoPressSendCheck || whatsappAutoPressSendCheck->isChecked();
        settings["open_delay_ms"] = 7000;
        settings["send_delay_ms"] = 10000;
        return settings;
    }

    void saveWhatsAppSettings() {
        try {
            writeJsonObject(appOutputPath("whatsapp_settings.json"), collectWhatsAppSettings());
            log("WhatsApp settings saved.");
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "WhatsApp Settings", ex.what());
        }
    }

    void openWhatsAppNumbers() {
        try {
            const auto settings = collectWhatsAppSettings();
            const QString messageFile = settings.value("message_file").toString();
            if (messageFile.isEmpty()) {
                QMessageBox::warning(this, "WhatsApp", "Please choose a WhatsApp message .txt file first.");
                return;
            }
            const QString message = readTextFileQt(messageFile);
            QStringList invalid;
            const QStringList numbers = parseWhatsAppNumbers(settings.value("numbers").toString(),
                                                             settings.value("country_code").toString("91"), &invalid);
            if (!invalid.isEmpty()) {
                QMessageBox::warning(this, "WhatsApp", "Some invalid numbers were skipped:\n" + invalid.mid(0, 8).join("\n"));
            }
            if (numbers.isEmpty()) {
                QMessageBox::warning(this, "WhatsApp", "Please enter at least one valid WhatsApp number.");
                return;
            }
            saveWhatsAppSettings();
            for (const QString& number : numbers) {
                const QString url = "https://wa.me/" + number + "?text=" + QString::fromUtf8(QUrl::toPercentEncoding(message));
                QDesktopServices::openUrl(QUrl(url));
            }
            log("Opened WhatsApp chats for " + QString::number(numbers.size()) + " number(s).");
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "WhatsApp", ex.what());
        }
    }

    QString buildClassReportText(const QString& className) {
        ensureDb();
        const auto rows = loadAttendanceRows(db, nowDate());
        QSet<QString> presentCodes;
        QStringList presentLines;
        QStringList lateLines;
        for (const auto& row : rows) {
            if (qstr(row.className) != className) {
                continue;
            }
            presentCodes.insert(qstr(row.studentCode));
            const QString line = qstr(row.studentCode) + " - " + qstr(row.name) + " - " + qstr(row.time) + " - " + qstr(row.status);
            if (row.status == "Late") {
                lateLines.append(line);
            } else {
                presentLines.append(line);
            }
        }
        QStringList absentLines;
        for (const auto& [label, student] : state.students) {
            (void)label;
            if (qstr(student.className) == className && !presentCodes.contains(qstr(student.studentCode))) {
                absentLines.append(qstr(student.studentCode) + " - " + qstr(student.name));
            }
        }
        if (presentLines.isEmpty() && lateLines.isEmpty() && absentLines.isEmpty()) {
            return "";
        }
        return className + " Attendance Summary - " + qstr(nowDate()) +
               "\n\nPresent: " + QString::number(presentLines.size()) +
               "\nLate: " + QString::number(lateLines.size()) +
               "\nAbsent: " + QString::number(absentLines.size()) +
               "\n\nPRESENT\n" + presentLines.join("\n") +
               "\n\nLATE\n" + lateLines.join("\n") +
               "\n\nABSENT\n" + absentLines.join("\n");
    }

    void sendManualWhatsAppGroups() {
        try {
            const auto settings = collectWhatsAppSettings();
            const QString messageFile = settings.value("message_file").toString();
            const QString manualMessage = messageFile.isEmpty() ? "" : readTextFileQt(messageFile);
            const QJsonObject groups = settings.value("class_groups").toObject();
            int opened = 0;
            for (const QString& className : classOrder()) {
                const auto checkIt = whatsappManualChecks.find(className);
                if (checkIt == whatsappManualChecks.end() || !checkIt->second->isChecked()) {
                    continue;
                }
                const QString groupName = groups.value(className).toString().trimmed();
                if (groupName.isEmpty()) {
                    continue;
                }
                QString message = manualMessage;
                if (message.isEmpty()) {
                    message = buildClassReportText(className);
                }
                QApplication::clipboard()->setText("Group: " + groupName + "\n\n" + message);
                QDesktopServices::openUrl(QUrl("https://web.whatsapp.com/"));
                ++opened;
            }
            saveWhatsAppSettings();
            log("Prepared WhatsApp group messages for " + QString::number(opened) + " class group(s). Clipboard contains the latest group/message.");
            QMessageBox::information(this, "WhatsApp Groups",
                                     "WhatsApp Web opened. For group messages, the selected group name and message are copied to clipboard.\n\n"
                                     "Paste/search in WhatsApp Web and send. Full browser-key automation can be added next if needed.");
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "WhatsApp Groups", ex.what());
        }
    }

    void loadServerSettings() {
        const QJsonObject settings = readJsonObject(appOutputPath("server_sync_settings.json"));
        if (serverUrlEdit) serverUrlEdit->setText(settings.value("sync_url").toString("https://attendance.savvymotherschool.com/api/sync.php"));
        if (serverApiKeyEdit) serverApiKeyEdit->setText(settings.value("api_key").toString());
    }

    QJsonObject collectServerSettings() const {
        QJsonObject settings;
        settings["sync_url"] = serverUrlEdit ? normalizeServerUrl(serverUrlEdit->text()) : "";
        settings["api_key"] = serverApiKeyEdit ? serverApiKeyEdit->text().trimmed() : "";
        return settings;
    }

    void saveServerSettings() {
        try {
            writeJsonObject(appOutputPath("server_sync_settings.json"), collectServerSettings());
            log("Server sync settings saved.");
        } catch (const std::exception& ex) {
            QMessageBox::critical(this, "Server Sync", ex.what());
        }
    }

    QJsonArray queryJsonArray(sqlite3* dbHandle, const char* sql, const QStringList& keys) {
        sqlite3_stmt* stmt = nullptr;
        QJsonArray rows;
        if (sqlite3_prepare_v2(dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return rows;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QJsonObject row;
            for (int i = 0; i < keys.size(); ++i) {
                if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER) {
                    row[keys[i]] = sqlite3_column_int(stmt, i);
                } else if (sqlite3_column_type(stmt, i) == SQLITE_FLOAT) {
                    row[keys[i]] = sqlite3_column_double(stmt, i);
                } else {
                    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    row[keys[i]] = text ? QString::fromUtf8(text) : "";
                }
            }
            rows.append(row);
        }
        sqlite3_finalize(stmt);
        return rows;
    }

    QJsonObject buildServerPayload() {
        ensureDb();
        QJsonObject payload;
        payload["school"] = "savvy_mother_international_school";
        payload["generated_at"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        payload["students"] = queryJsonArray(
            db,
            "SELECT student_code, name, class_name, section, roll_number, label_id, COALESCE(created_at, '') FROM school_students ORDER BY class_name, section, name;",
            {"student_code", "name", "class_name", "section", "roll_number", "label_id", "created_at"});
        payload["attendance"] = queryJsonArray(
            db,
            "SELECT student_code, name, class_name, section, date, time, status, confidence FROM school_attendance ORDER BY date, time, class_name, name;",
            {"student_code", "name", "class_name", "section", "date", "time", "status", "confidence"});
        return payload;
    }

    void syncServerNow() {
        if (!beginProcess("Server sync")) {
            return;
        }
        const QJsonObject settings = collectServerSettings();
        const QString url = settings.value("sync_url").toString();
        const QString apiKey = settings.value("api_key").toString();
        if (url.isEmpty() || apiKey.isEmpty()) {
            endProcess("Server sync cancelled.");
            QMessageBox::warning(this, "Server Sync", "Please enter both sync URL and API key.");
            return;
        }
        QByteArray body;
        try {
            saveServerSettings();
            body = QJsonDocument(buildServerPayload()).toJson(QJsonDocument::Compact);
        } catch (const std::exception& ex) {
            endProcess("Server sync failed.");
            QMessageBox::critical(this, "Server Sync", ex.what());
            return;
        }
        auto* manager = new QNetworkAccessManager(this);
        QNetworkRequest request{QUrl(url)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("X-API-Key", apiKey.toUtf8());
        request.setRawHeader("User-Agent", "SavvyAttendanceDesktop/1.0");
        QNetworkReply* reply = manager->post(request, body);
        log("Server sync started...");
        connect(reply, &QNetworkReply::finished, this, [this, reply, manager] {
            const QByteArray response = reply->readAll();
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || statusCode >= 400) {
                log("Server sync failed: " + reply->errorString());
                QMessageBox::critical(this, "Server Sync", "Server sync failed:\n" + reply->errorString() + "\n\n" + QString::fromUtf8(response.left(500)));
            } else {
                log("Server sync completed.");
                QMessageBox::information(this, "Server Sync", "Server sync completed.\n\n" + QString::fromUtf8(response.left(500)));
            }
            reply->deleteLater();
            manager->deleteLater();
            endProcess();
        });
    }

    void openServerDashboard() {
        const QString syncUrl = normalizeServerUrl(serverUrlEdit ? serverUrlEdit->text() : "");
        QUrl url(syncUrl);
        url.setPath("/");
        QDesktopServices::openUrl(url.isValid() ? url : QUrl("https://attendance.savvymotherschool.com/"));
    }

    void startCoreLoad() {
        if (!beginProcess("Loading face model")) {
            return;
        }
        modelLabel->setText("Face model: loading...");
        auto* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
            watcher->deleteLater();
            if (state.ready) {
                modelLabel->setText("Face model: ready");
                endProcess("Existing trained model loaded.");
            } else {
                modelLabel->setText("Face model: not loaded");
                endProcess(qstr(state.errorText));
                QMessageBox::warning(this, "Face Model Not Loaded", qstr(state.errorText));
            }
        });
        watcher->setFuture(QtConcurrent::run([this] { loadResources(state); }));
    }

    void trainModelAsync() {
        if (!beginProcess("Training model")) {
            return;
        }
        log("Training started. Please wait...");
        auto* watcher = new QFutureWatcher<int>(this);
        connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher] {
            const int result = watcher->result();
            watcher->deleteLater();
            if (result == 0) {
                log("Training complete. Reloading model...");
                endProcess();
                startCoreLoad();
            } else {
                const QString message = gLastTrainingError.empty()
                    ? "Training failed. Existing model kept unchanged."
                    : qstr(gLastTrainingError);
                endProcess("Training failed. Existing model kept unchanged.");
                QMessageBox::critical(this, "Training Error", message);
            }
        });
        watcher->setFuture(QtConcurrent::run([] { return trainModelFromDataset(); }));
    }

    void registerStudent() {
        if (nameEdit->text().trimmed().isEmpty() || codeEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "Register Student", "Name and Student Code are required.");
            return;
        }
        if (!beginProcess("Student registration", true)) {
            return;
        }
        try {
            Student student;
            student.labelId = nextLabelIdFromFile();
            student.name = str(nameEdit->text());
            student.studentCode = str(codeEdit->text());
            student.className = str(classEdit->currentText());
            student.section = str(sectionEdit->text());
            student.rollNumber = str(rollEdit->text());

            ensureDb();
            saveStudentRecord(db, student);
            appendStudentLabel(student);
            state.students[student.labelId] = student;

            const auto datasetDir = appOutputPath(kDatasetPath) /
                (safeFolderText(student.studentCode) + "_" + safeFolderText(student.name));
            std::filesystem::create_directories(datasetDir);

            cv::VideoCapture cap = openCameraIndex(cameraIndexSpin->value());
            int saved = 0;
            const auto start = std::chrono::steady_clock::now();
            while (saved < 30 && std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < 45) {
                cv::Mat frame;
                if (!cap.read(frame) || frame.empty()) {
                    continue;
                }
                cv::Mat gray;
                cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                std::vector<cv::Rect> faces;
                state.detector.detectMultiScale(enhanceGrayImage(gray), faces, 1.2, 5, 0, cv::Size(60, 60));
                if (faces.empty()) {
                    continue;
                }
                std::sort(faces.begin(), faces.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.area() > b.area(); });
                cv::Mat face = cropFaceWithPadding(frame, faces.front());
                if (face.empty()) {
                    continue;
                }
                cv::imwrite((datasetDir / ("img_" + std::to_string(saved + 1) + ".jpg")).string(), face);
                ++saved;
                cv::waitKey(80);
            }
            cap.release();
            endProcess("Registered " + qstr(student.name) + ". Captured " + QString::number(saved) + " face images.");
            QMessageBox::information(this, "Register Student", "Student saved. Please train the model now.");
        } catch (const std::exception& ex) {
            endProcess("Student registration failed.");
            QMessageBox::critical(this, "Register Student", ex.what());
        }
    }

    void startAttendance() {
        if (!state.ready) {
            QMessageBox::warning(this, "Attendance", "Face model is not loaded yet. Train/load model first.");
            return;
        }
        if (!beginProcess("Attendance camera", true)) {
            return;
        }
        try {
            ensureDb();
            loadStudentExtras(db, state.students);
            arrivalSettings = loadArrivalSettings();
            refreshMarkedToday();
            camera = openCameraIndex(cameraIndexSpin->value());
            if (stopCameraButton != nullptr) {
                stopCameraButton->setEnabled(true);
            }
            attendanceMode = true;
            candidate = {};
            unknownCandidateCount = 0;
            lastRecognized.clear();
            cameraTimer.start(30);
            log("Attendance camera started. Already marked today: " + QString::number(markedTodayCodes.size()) + " students.");
        } catch (const std::exception& ex) {
            endProcess("Attendance camera failed.");
            QMessageBox::critical(this, "Attendance", ex.what());
        }
    }

    void startPreview() {
        if (!beginProcess("Camera preview", true)) {
            return;
        }
        try {
            camera = openCameraIndex(cameraIndexSpin->value());
            if (stopCameraButton != nullptr) {
                stopCameraButton->setEnabled(true);
            }
            attendanceMode = false;
            previewTimer.start(30);
            log("Camera preview started.");
        } catch (const std::exception& ex) {
            endProcess("Camera preview failed.");
            QMessageBox::critical(this, "Camera Preview", ex.what());
        }
    }

    void stopCamera() {
        const bool hadCamera = camera.isOpened() || cameraTimer.isActive() || previewTimer.isActive();
        cameraTimer.stop();
        previewTimer.stop();
        if (camera.isOpened()) {
            camera.release();
        }
        attendanceMode = false;
        if (hadCamera && processBusy &&
            (activeProcessName == "Attendance camera" || activeProcessName == "Camera preview" || activeProcessName == "Student registration")) {
            endProcess("Camera stopped.");
        } else {
            updateActionButtons();
        }
    }

    void processPreviewFrame() {
        cv::Mat frame;
        if (!camera.read(frame) || frame.empty()) {
            return;
        }
        videoLabel->setPixmap(QPixmap::fromImage(matToImage(frame)).scaled(videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    std::vector<std::pair<int, std::filesystem::path>> getUnknownFaceFiles() {
        const auto unknownDir = appOutputPath(kUnknownFacesPath);
        std::filesystem::create_directories(unknownDir);
        std::vector<std::pair<int, std::filesystem::path>> files;
        for (const auto& entry : std::filesystem::directory_iterator(unknownDir)) {
            if (!entry.is_regular_file() || !hasImageExtension(entry.path())) {
                continue;
            }
            const std::string stem = entry.path().stem().string();
            if (stem.rfind("unknown", 0) != 0) {
                continue;
            }
            try {
                files.push_back({std::stoi(stem.substr(7)), entry.path()});
            } catch (...) {
            }
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        return files;
    }

    bool loadUnknownFaceRecognizer(bool force = false) {
        if (unknownFacesLoaded && !force) {
            return unknownFaceCount > 0;
        }

        std::vector<cv::Mat> faces;
        std::vector<int> labels;
        for (const auto& [label, path] : getUnknownFaceFiles()) {
            cv::Mat image = cv::imread(path.string(), cv::IMREAD_COLOR);
            if (image.empty()) {
                continue;
            }
            cv::Mat face = preprocessFace(image);
            if (face.empty()) {
                continue;
            }
            faces.push_back(face);
            labels.push_back(label);
        }

        unknownRecognizer = cv::face::LBPHFaceRecognizer::create();
        unknownFaceCount = static_cast<int>(faces.size());
        unknownFacesLoaded = true;
        if (faces.empty()) {
            return false;
        }

        unknownRecognizer->train(faces, labels);
        return true;
    }

    bool unknownFaceAlreadySaved(const cv::Mat& face) {
        if (!loadUnknownFaceRecognizer(false)) {
            return false;
        }
        int label = -1;
        double confidence = 9999.0;
        unknownRecognizer->predict(face, label, confidence);
        return label >= 0 && confidence < kUnknownFaceMatchThreshold;
    }

    bool saveUnknownFaceIfNew(const cv::Mat& rawFace) {
        if (rawFace.empty() || rawFace.cols < 60 || rawFace.rows < 60) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastUnknownSavedTime).count() < 10) {
            return false;
        }

        const auto unknownDir = appOutputPath(kUnknownFacesPath);
        std::filesystem::create_directories(unknownDir);

        const std::string dateStr = nowDate();
        const std::string timeStr = nowClock();
        std::string safeTime = timeStr;
        for (char& c : safeTime) {
            if (c == ':') c = '-';
        }

        int next = 1;
        const auto files = getUnknownFaceFiles();
        if (!files.empty()) {
            next = files.back().first + 1;
        }
        const auto fileName = "unknown" + std::to_string(next) + "_" + safeTime + ".jpg";
        const auto path = unknownDir / fileName;
        if (!cv::imwrite(path.string(), rawFace)) {
            return false;
        }

        std::ofstream logFile(unknownDir / "unknown_log.csv", std::ios::app);
        logFile << fileName << ',' << dateStr << ',' << timeStr << ',' << path.string() << '\n';

        lastUnknownSavedTime = now;
        return true;
    }

    void playMarkedBeep() {
#ifdef _WIN32
        const auto beepPath = findApplicationAssetFile("beep.wav");
        if (!beepPath.empty()) {
            PlaySoundA(beepPath.string().c_str(), nullptr, SND_FILENAME | SND_ASYNC);
            return;
        }
#endif
        QApplication::beep();
    }

    void processAttendanceFrame() {
        cv::Mat frame;
        if (!camera.read(frame) || frame.empty()) {
            return;
        }

        if (nowDate() != currentAttendanceDate) {
            refreshMarkedToday();
        }

        const auto faces = detectFaces(state, frame);
        if (!faces.empty()) {
            const cv::Rect faceRect = faces.front();
            if (faceRect.width >= kMinFaceWidth && faceRect.height >= kMinFaceHeight) {
                int label = -1;
                double confidence = 9999.0;
                bool recognized = false;
                const cv::Mat rawFaceForEmbedding = cropFaceWithPadding(frame, faceRect);

                if (const auto embeddingMatch = recognizeByEmbedding(state, rawFaceForEmbedding)) {
                    label = embeddingMatch->first;
                    confidence = (1.0 - embeddingMatch->second) * 100.0;
                    recognized = state.students.count(label) > 0;
                } else if (!state.recognizer.empty()) {
                    cv::Mat gray;
                    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                    cv::Mat face = preprocessFace(cropFaceWithPadding(gray, faceRect));
                    if (!face.empty()) {
                        state.recognizer->predict(face, label, confidence);
                        recognized = confidence < kConfidenceThreshold && state.students.count(label) > 0;
                    }
                }

                if (recognized) {
                    unknownCandidateCount = 0;
                    const auto& student = state.students.at(label);
                    const auto now = std::chrono::steady_clock::now();
                    const bool alreadyMarked = markedTodayCodes.count(student.studentCode) > 0;

                    if (alreadyMarked) {
                        // Person comes twice: already marked today!
                        // Cannot remember / mark them again. Silent pass, NO BEEP.
                        cv::rectangle(frame, faceRect, cv::Scalar(255, 200, 0), 2);
                        cv::putText(frame, student.name + " (Already Marked)",
                                    cv::Point(faceRect.x, std::max(28, faceRect.y - 8)),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(255, 200, 0), 2, cv::LINE_AA);
                        candidate = {};
                    } else {
                        // First arrival today: Mark attendance with ONE BEEP
                        cv::rectangle(frame, faceRect, cv::Scalar(0, 255, 0), 2);
                        cv::putText(frame, student.name,
                                    cv::Point(faceRect.x, std::max(28, faceRect.y - 8)),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);

                        const bool sameCandidate = candidate.labelId == label &&
                            std::chrono::duration<double>(now - candidate.lastSeen).count() <= kConfirmWindowSeconds;
                        if (sameCandidate) {
                            candidate.count += 1;
                            candidate.bestConfidence = std::min(candidate.bestConfidence, confidence);
                        } else {
                            candidate.labelId = label;
                            candidate.count = 1;
                            candidate.bestConfidence = confidence;
                        }
                        candidate.lastSeen = now;

                        const auto lastIt = lastRecognized.find(student.studentCode);
                        const bool cooldownPassed = lastIt == lastRecognized.end() ||
                            std::chrono::duration_cast<std::chrono::seconds>(now - lastIt->second).count() >= kCooldownSeconds;

                        if (candidate.count >= kConfirmFrames && cooldownPassed) {
                            std::string message;
                            const std::string markStatus = attendanceStatusForTime(student, nowClock(), arrivalSettings);
                            if (saveAttendance(db, student, candidate.bestConfidence, markStatus, message)) {
                                markedTodayCodes.insert(student.studentCode);
                                lastRecognized[student.studentCode] = now;
                                playMarkedBeep();
                            }
                            log(qstr(message));
                            candidate = {};
                        }
                    }
                } else {
                    // Face detected but not recognized in 350+ dataset: Unknown visitor!
                    candidate = {};
                    cv::rectangle(frame, faceRect, cv::Scalar(0, 0, 255), 2);
                    cv::putText(frame, "Unknown",
                                cv::Point(faceRect.x, std::max(28, faceRect.y - 8)),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);

                    unknownCandidateCount += 1;
                    if (unknownCandidateCount >= 2) {
                        if (saveUnknownFaceIfNew(cropFaceWithPadding(frame, faceRect))) {
                            log("New unknown visitor face captured & saved.");
                        }
                    }
                }
            }
        } else {
            unknownCandidateCount = 0;
        }

        videoLabel->setPixmap(QPixmap::fromImage(matToImage(frame)).scaled(
            videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    void saveReports() {
        if (!beginProcess("Saving reports")) {
            return;
        }
        try {
            ensureDb();
            saveDailyReports(db, state.students);
            const QString folder = autosaveEdit->text().trimmed();
            if (!folder.isEmpty()) {
                const auto source = appOutputPath(compactDateForFolder());
                const auto target = std::filesystem::path(folder.toStdString()) / compactDateForFolder();
                std::filesystem::create_directories(target);
                if (std::filesystem::is_directory(source)) {
                    for (const auto& entry : std::filesystem::directory_iterator(source)) {
                        if (entry.is_regular_file()) {
                            std::filesystem::copy_file(entry.path(), target / entry.path().filename(),
                                                       std::filesystem::copy_options::overwrite_existing);
                        }
                    }
                }
            }
            endProcess("Reports saved.");
        } catch (const std::exception& ex) {
            endProcess("Report save failed.");
            QMessageBox::critical(this, "Reports", ex.what());
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        AttendanceWindow window;
        window.showMaximized();
        return app.exec();
    } catch (const std::exception& ex) {
        QMessageBox::critical(nullptr, "Savvy Attendance Startup Error", ex.what());
        return 1;
    } catch (...) {
        QMessageBox::critical(nullptr, "Savvy Attendance Startup Error", "Unknown startup error.");
        return 1;
    }
}
