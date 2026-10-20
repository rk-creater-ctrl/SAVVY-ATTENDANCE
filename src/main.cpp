#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/face.hpp>
#include <opencv2/objdetect.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kTrainerFile = "trainer.yml.gz";
constexpr const char* kLegacyTrainerFile = "trainer.yml";
constexpr const char* kEmbeddingModelFile = "face_embedding.onnx";
constexpr const char* kEmbeddingDbFile = "face_embeddings.yml.gz";
constexpr const char* kYuNetModelFile = "face_detection_yunet_2023mar.onnx";
constexpr const char* kLabelsFile = "labels.txt";
constexpr const char* kDbFile = "attendance.db";
constexpr const char* kDatasetPath = "dataset";
constexpr const char* kReportsPath = "reports";
constexpr const char* kUnknownFacesPath = "unknown_faces";
constexpr const char* kInstalledModeFile = "installed_mode.txt";
constexpr const char* kCascadeFile = "haarcascade_frontalface_default.xml";
constexpr const char* kLogoFile = "school_logo.webp";
constexpr const char* kArrivalSettingsFile = "arrival_settings.json";
constexpr int kConfidenceThreshold = 45;
constexpr int kMinFaceWidth = 60;
constexpr int kMinFaceHeight = 60;
constexpr int kCooldownSeconds = 3;
constexpr int kConfirmFrames = 2;
constexpr int kPoorLightMeanThreshold = 35;
constexpr int kMaxTrainingImagesPerStudent = 3;
constexpr int kMaxEmbeddingImagesPerStudent = 20;
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 640;
constexpr double kConfirmWindowSeconds = 2.0;
constexpr double kEmbeddingMinSimilarity = 0.52;
constexpr double kEmbeddingMinMargin = 0.03;

struct Student {
    int labelId = -1;
    std::string name;
    std::string studentCode;
    std::string className;
    std::string section;
    std::string rollNumber;
};

struct AttendanceRow {
    std::string studentCode;
    std::string name;
    std::string className;
    std::string section;
    std::string date;
    std::string time;
    std::string status;
};

struct ArrivalSettings {
    std::string studentArrivalTime;
    std::string staffArrivalTime;
};

struct RecognizedCandidate {
    int labelId = -1;
    int count = 0;
    double bestConfidence = 9999.0;
    std::chrono::steady_clock::time_point lastSeen{};
};

struct EmbeddingTemplate {
    int labelId = -1;
    cv::Mat embedding;
};

struct AppState {
    std::atomic<bool> loading{true};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    mutable std::mutex statusMutex;
    std::string statusText = "Loading model...";
    std::string errorText;
    std::map<int, Student> students;
    cv::Ptr<cv::face::LBPHFaceRecognizer> recognizer;
    cv::dnn::Net embeddingNet;
    std::vector<EmbeddingTemplate> embeddingTemplates;
    bool embeddingReady = false;
    cv::CascadeClassifier detector;
    cv::Ptr<cv::FaceDetectorYN> yunetDetector;
    bool yunetReady = false;
    cv::Mat splashLogo;
};

enum class AppMode {
    Loading,
    Home,
    Attendance,
    Training
};

enum class UiClickAction {
    None = 0,
    StartAttendance = 1,
    TrainModel = 2,
    SaveReports = 3,
    ExitApp = 4,
    GoHome = 5
};

std::atomic<int> gPendingClickAction{static_cast<int>(UiClickAction::None)};
std::atomic<int> gMouseX{-1};
std::atomic<int> gMouseY{-1};
std::filesystem::path gAppDir = std::filesystem::current_path();
std::filesystem::path gDataDir = std::filesystem::current_path();
std::string gLastTrainingError;

const cv::Rect kHomeStartButton(110, 360, 340, 92);
const cv::Rect kHomeTrainButton(510, 360, 340, 92);
const cv::Rect kHomeReportsButton(110, 480, 340, 92);
const cv::Rect kHomeExitButton(510, 480, 340, 92);
const cv::Rect kAttendanceExitButton(kWindowWidth - 190, 18, 160, 42);
const cv::Rect kAttendanceHomeButton(kWindowWidth - 190, 70, 160, 42);

std::filesystem::path appOutputPath(const std::string& fileName);
void ensureRuntimeLayout();
std::filesystem::path chooseInstalledDataDir();
void setRuntimeDataRoot(const std::filesystem::path& dataRoot);
std::filesystem::path labelsPath();
std::filesystem::path datasetPath();
std::uintmax_t safeFileSize(const std::filesystem::path& path);
int countSupportedDatasetImages();
std::string startupDiagnosticsText();
void writeStartupDiagnostics();

bool pointInRect(int x, int y, const cv::Rect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

UiClickAction takePendingClickAction() {
    return static_cast<UiClickAction>(
        gPendingClickAction.exchange(static_cast<int>(UiClickAction::None)));
}

void onMouseEvent(int event, int x, int y, int, void*) {
    gMouseX = x;
    gMouseY = y;
    if (event != cv::EVENT_LBUTTONUP) {
        return;
    }

    if (pointInRect(x, y, kHomeStartButton)) {
        gPendingClickAction = static_cast<int>(UiClickAction::StartAttendance);
    } else if (pointInRect(x, y, kHomeTrainButton)) {
        gPendingClickAction = static_cast<int>(UiClickAction::TrainModel);
    } else if (pointInRect(x, y, kHomeReportsButton)) {
        gPendingClickAction = static_cast<int>(UiClickAction::SaveReports);
    } else if (pointInRect(x, y, kHomeExitButton) || pointInRect(x, y, kAttendanceExitButton)) {
        gPendingClickAction = static_cast<int>(UiClickAction::ExitApp);
    } else if (pointInRect(x, y, kAttendanceHomeButton)) {
        gPendingClickAction = static_cast<int>(UiClickAction::GoHome);
    }
}

std::filesystem::path cleanPath(const std::filesystem::path& path) {
    try {
        return std::filesystem::weakly_canonical(path);
    } catch (...) {
        return std::filesystem::absolute(path);
    }
}

void initializeRuntimePaths(const char* argv0) {
    std::filesystem::path exePath = argv0 != nullptr ? std::filesystem::path(argv0) : std::filesystem::path();
    if (exePath.empty()) {
        gAppDir = std::filesystem::current_path();
        gDataDir = chooseInstalledDataDir();
        ensureRuntimeLayout();
        return;
    }
    if (exePath.is_relative()) {
        exePath = std::filesystem::absolute(exePath);
    }
    gAppDir = cleanPath(exePath).parent_path();
    gDataDir = chooseInstalledDataDir();
    if (!gDataDir.empty()) {
        std::filesystem::create_directories(gDataDir);
    }
    ensureRuntimeLayout();
}

std::vector<std::filesystem::path> runtimeSearchDirs() {
    std::vector<std::filesystem::path> dirs;
    auto addDir = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        try {
            const auto cleaned = cleanPath(p);
            if (std::filesystem::is_directory(cleaned)) {
                for (const auto& d : dirs) {
                    if (std::filesystem::equivalent(d, cleaned)) return;
                }
                dirs.push_back(cleaned);
            }
        } catch (...) {}
    };

    addDir(gDataDir);
    addDir(gAppDir);
    addDir(gAppDir.parent_path());
    addDir(gAppDir.parent_path() / "Release");
    addDir(std::filesystem::current_path());
    addDir(std::filesystem::path("C:/Users/Public/Documents/SavvyAttendanceSystem"));
    return dirs;
}

std::filesystem::path findExistingFile(const std::string& fileName) {
    for (const auto& dir : runtimeSearchDirs()) {
        const auto path = dir / fileName;
        if (std::filesystem::is_regular_file(path)) {
            return path;
        }
    }
    return {};
}

std::filesystem::path findApplicationAssetFile(const std::string& fileName) {
    const std::vector<std::filesystem::path> candidates = {
        gAppDir / fileName,
        gDataDir / fileName
    };
    for (const auto& candidate : candidates) {
        const auto path = cleanPath(candidate);
        if (std::filesystem::is_regular_file(path)) {
            return path;
        }
    }
    return {};
}

std::filesystem::path findNewestFile(const std::string& fileName) {
    const auto path = appOutputPath(fileName);
    if (std::filesystem::is_regular_file(path)) {
        return path;
    }
    return {};
}

std::filesystem::path findExistingDirectory(const std::string& dirName) {
    for (const auto& dir : runtimeSearchDirs()) {
        const auto path = dir / dirName;
        if (std::filesystem::is_directory(path)) {
            return path;
        }
    }
    return {};
}

std::filesystem::path appOutputPath(const std::string& fileName) {
    return cleanPath(gDataDir / fileName);
}

std::filesystem::path chooseInstalledDataDir() {
    if (const char* publicDir = std::getenv("PUBLIC")) {
        return cleanPath(std::filesystem::path(publicDir) / "Documents" / "SavvyAttendanceSystem");
    }
    return cleanPath(std::filesystem::path("C:\\Users\\Public") / "Documents" / "SavvyAttendanceSystem");
}

void setRuntimeDataRoot(const std::filesystem::path& dataRoot) {
    gDataDir = cleanPath(dataRoot);
    std::filesystem::create_directories(gDataDir);
    ensureRuntimeLayout();
}

void ensureEmptyFile(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        return;
    }
    std::ofstream file(path);
}

void ensureRuntimeLayout() {
    std::filesystem::create_directories(appOutputPath(kDatasetPath));
    std::filesystem::create_directories(appOutputPath(kReportsPath));
    std::filesystem::create_directories(appOutputPath(kUnknownFacesPath));
    const auto targetLabels = appOutputPath(kLabelsFile);
    if (!std::filesystem::exists(targetLabels) || safeFileSize(targetLabels) == 0) {
        for (const auto& dir : runtimeSearchDirs()) {
            const auto src = dir / kLabelsFile;
            if (safeFileSize(src) > 0) {
                try {
                    if (!std::filesystem::exists(targetLabels) || !std::filesystem::equivalent(src, targetLabels)) {
                        std::filesystem::copy_file(src, targetLabels, std::filesystem::copy_options::overwrite_existing);
                        break;
                    }
                } catch (...) {
                }
            }
        }
    }
}

std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        parts.push_back(trim(item));
    }
    return parts;
}

std::map<int, Student> loadStudents() {
    const auto labelsPathValue = labelsPath();
    if (!std::filesystem::is_regular_file(labelsPathValue)) {
        ensureEmptyFile(labelsPathValue);
        return {};
    }
    if (safeFileSize(labelsPathValue) == 0) {
        return {};
    }

    std::ifstream input(labelsPathValue);
    if (!input) {
        throw std::runtime_error("Could not open labels.txt: " + labelsPathValue.string());
    }

    std::map<int, Student> students;
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = splitCsvLine(line);
        if (parts.size() != 4) {
            continue;
        }

        Student student;
        student.labelId = std::stoi(parts[0]);
        student.name = parts[1];
        student.studentCode = parts[2];
        student.className = parts[3];
        students[student.labelId] = student;
    }
    return students;
}

std::string getTrainerPath() {
    const auto trainerPath = appOutputPath(kTrainerFile);
    if (std::filesystem::is_regular_file(trainerPath)) {
        return trainerPath.string();
    }

    const auto legacyTrainerPath = appOutputPath(kLegacyTrainerFile);
    if (std::filesystem::is_regular_file(legacyTrainerPath)) {
        return legacyTrainerPath.string();
    }

    throw std::runtime_error("Could not find trainer model.\n\nSearched:\n" +
                             trainerPath.string() + "\n" + legacyTrainerPath.string());
}

std::string nowDate() {
    const std::time_t t = std::time(nullptr);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &t);
#else
    localtime_r(&t, &localTime);
#endif
    std::ostringstream out;
    out << std::put_time(&localTime, "%Y-%m-%d");
    return out.str();
}

std::string nowClock() {
    const std::time_t t = std::time(nullptr);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &t);
#else
    localtime_r(&t, &localTime);
#endif
    std::ostringstream out;
    out << std::put_time(&localTime, "%H:%M:%S");
    return out.str();
}

std::string compactDateForFolder() {
    return (std::filesystem::path(kReportsPath) / (nowDate() + "-report")).string();
}

std::optional<std::string> readTextFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string extractJsonString(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const auto keyPos = json.find(marker);
    if (keyPos == std::string::npos) {
        return "";
    }
    const auto colonPos = json.find(':', keyPos + marker.size());
    if (colonPos == std::string::npos) {
        return "";
    }
    const auto firstQuote = json.find('"', colonPos + 1);
    if (firstQuote == std::string::npos) {
        return "";
    }
    const auto secondQuote = json.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) {
        return "";
    }
    return json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

ArrivalSettings loadArrivalSettings() {
    ArrivalSettings settings;
    const auto settingsPath = findNewestFile(kArrivalSettingsFile);
    if (settingsPath.empty()) {
        return settings;
    }
    const auto json = readTextFile(settingsPath.string());
    if (!json) {
        return settings;
    }
    settings.studentArrivalTime = extractJsonString(*json, "student_arrival_time");
    settings.staffArrivalTime = extractJsonString(*json, "staff_arrival_time");
    return settings;
}

std::optional<int> clockToMinutes(const std::string& value) {
    if (value.size() < 4) {
        return std::nullopt;
    }
    const auto colon = value.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    try {
        const int hour = std::stoi(value.substr(0, colon));
        const int minute = std::stoi(value.substr(colon + 1, 2));
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            return std::nullopt;
        }
        return hour * 60 + minute;
    } catch (...) {
        return std::nullopt;
    }
}

std::string attendanceStatusForTime(const Student& student, const std::string& timeText,
                                    const ArrivalSettings& settings) {
    const std::string cutoff = student.className == "Staff" ? settings.staffArrivalTime : settings.studentArrivalTime;
    const auto cutoffMinutes = clockToMinutes(cutoff);
    const auto arrivalMinutes = clockToMinutes(timeText);
    if (!cutoffMinutes || !arrivalMinutes) {
        return "Present";
    }
    return *arrivalMinutes > *cutoffMinutes ? "Late" : "Present";
}

std::string csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped += ch;
        }
    }
    escaped += '"';
    return escaped;
}

void setStatus(AppState& state, const std::string& status) {
    std::lock_guard<std::mutex> lock(state.statusMutex);
    state.statusText = status;
}

std::string getStatus(const AppState& state) {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(state.statusMutex));
    return state.statusText;
}

cv::Mat enhanceGrayImage(const cv::Mat& gray) {
    cv::Mat gray8 = gray;
    if (gray.channels() == 3) {
        cv::cvtColor(gray, gray8, cv::COLOR_BGR2GRAY);
    }
    if (gray8.type() != CV_8U) {
        cv::normalize(gray8, gray8, 0, 255, cv::NORM_MINMAX, CV_8U);
    }

    cv::Mat background;
    cv::GaussianBlur(gray8, background, cv::Size(0, 0), 25.0, 25.0);
    background = cv::max(background, 1);

    cv::Mat normalized;
    cv::divide(gray8, background, normalized, 128.0);
    cv::normalize(normalized, normalized, 0, 255, cv::NORM_MINMAX, CV_8U);

    auto clahe = cv::createCLAHE(2.5, cv::Size(8, 8));
    cv::Mat enhanced;
    clahe->apply(normalized, enhanced);
    return enhanced;
}

cv::Mat cropFaceWithPadding(const cv::Mat& image, const cv::Rect& rect, double paddingRatio = 0.18) {
    if (image.empty() || rect.width <= 0 || rect.height <= 0) {
        return {};
    }

    const double centerX = rect.x + rect.width / 2.0;
    const double centerY = rect.y + rect.height / 2.0;
    const double side = std::max(rect.width, rect.height) * (1.0 + paddingRatio * 2.0);

    int x1 = static_cast<int>(std::round(centerX - side / 2.0));
    int y1 = static_cast<int>(std::round(centerY - side / 2.0));
    int x2 = static_cast<int>(std::round(centerX + side / 2.0));
    int y2 = static_cast<int>(std::round(centerY + side / 2.0));

    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(image.cols, x2);
    y2 = std::min(image.rows, y2);

    if (x2 <= x1 || y2 <= y1) {
        return {};
    }
    return image(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
}

cv::Mat neutralizeFaceBackground(const cv::Mat& gray) {
    if (gray.empty()) {
        return {};
    }

    cv::Mat mask = cv::Mat::zeros(gray.size(), CV_8U);
    const cv::Point center(gray.cols / 2, gray.rows / 2);
    const cv::Size axes(std::max(1, static_cast<int>(gray.cols * 0.44)),
                        std::max(1, static_cast<int>(gray.rows * 0.49)));
    cv::ellipse(mask, center, axes, 0, 0, 360, cv::Scalar(255), -1);

    const double fillValue = cv::mean(gray, mask)[0];
    cv::Mat neutral(gray.size(), gray.type(), cv::Scalar(fillValue));
    gray.copyTo(neutral, mask);
    return neutral;
}

cv::Mat preprocessFace(const cv::Mat& grayFace) {
    if (grayFace.empty()) {
        return {};
    }
    cv::Mat resized;
    cv::resize(grayFace, resized, cv::Size(200, 200), 0, 0,
               grayFace.cols > 200 || grayFace.rows > 200 ? cv::INTER_AREA : cv::INTER_CUBIC);
    cv::Mat enhanced = enhanceGrayImage(resized);
    return neutralizeFaceBackground(enhanced);
}

cv::Mat normalizeEmbeddingRow(const cv::Mat& embedding) {
    if (embedding.empty()) {
        return {};
    }
    cv::Mat row = embedding.reshape(1, 1);
    row.convertTo(row, CV_32F);
    const double norm = cv::norm(row, cv::NORM_L2);
    if (norm <= 1e-9) {
        return {};
    }
    return row / norm;
}

cv::Mat extractFaceEmbedding(cv::dnn::Net& net, const cv::Mat& faceImage) {
    if (net.empty() || faceImage.empty()) {
        return {};
    }
    cv::Mat color;
    if (faceImage.channels() == 1) {
        cv::cvtColor(faceImage, color, cv::COLOR_GRAY2BGR);
    } else {
        color = faceImage;
    }
    cv::Mat resized;
    cv::resize(color, resized, cv::Size(112, 112), 0, 0, cv::INTER_AREA);
    cv::Mat blob = cv::dnn::blobFromImage(resized, 1.0 / 128.0, cv::Size(112, 112),
                                          cv::Scalar(127.5, 127.5, 127.5), true, false);
    net.setInput(blob);
    return normalizeEmbeddingRow(net.forward());
}

cv::Mat chooseFaceForEmbedding(const cv::Mat& image, cv::CascadeClassifier* detector = nullptr) {
    if (image.empty()) {
        return {};
    }
    if (image.cols <= 400 && image.rows <= 400) {
        return image.clone();
    }
    if (detector == nullptr || detector->empty()) {
        return image.clone();
    }

    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
    }
    std::vector<cv::Rect> faces;
    detector->detectMultiScale(enhanceGrayImage(gray), faces, 1.2, 5, 0, cv::Size(60, 60));
    if (faces.empty()) {
        return image.clone();
    }
    std::sort(faces.begin(), faces.end(), [](const cv::Rect& a, const cv::Rect& b) {
        return a.area() > b.area();
    });
    return cropFaceWithPadding(image, faces.front());
}

bool embeddingFilesReady() {
    return !findExistingFile(kEmbeddingModelFile).empty() && !findExistingFile(kEmbeddingDbFile).empty();
}

std::optional<std::pair<int, double>> recognizeByEmbedding(const AppState& state, const cv::Mat& faceImage,
                                                           double* marginOut = nullptr) {
    if (!state.embeddingReady || state.embeddingNet.empty() || state.embeddingTemplates.empty()) {
        return std::nullopt;
    }

    cv::dnn::Net net = state.embeddingNet;
    cv::Mat query = extractFaceEmbedding(net, faceImage);
    if (query.empty()) {
        return std::nullopt;
    }

    int bestLabel = -1;
    double bestSimilarity = -1.0;
    double secondSimilarity = -1.0;
    for (const auto& item : state.embeddingTemplates) {
        if (item.embedding.empty()) {
            continue;
        }
        const double similarity = query.dot(item.embedding);
        if (similarity > bestSimilarity) {
            secondSimilarity = bestSimilarity;
            bestSimilarity = similarity;
            bestLabel = item.labelId;
        } else if (similarity > secondSimilarity) {
            secondSimilarity = similarity;
        }
    }

    const double margin = bestSimilarity - secondSimilarity;
    if (marginOut != nullptr) {
        *marginOut = margin;
    }
    if (bestLabel >= 0 && bestSimilarity >= kEmbeddingMinSimilarity && margin >= kEmbeddingMinMargin) {
        return std::make_pair(bestLabel, bestSimilarity);
    }
    return std::nullopt;
}

std::vector<cv::Rect> detectFaces(AppState& state, const cv::Mat& frame) {
    std::vector<cv::Rect> faces;
    if (frame.empty()) {
        return faces;
    }
    if (state.yunetReady && !state.yunetDetector.empty()) {
        try {
            state.yunetDetector->setInputSize(frame.size());
            cv::Mat facesMat;
            state.yunetDetector->detect(frame, facesMat);
            if (!facesMat.empty() && facesMat.rows > 0) {
                for (int i = 0; i < facesMat.rows; ++i) {
                    const float x = facesMat.at<float>(i, 0);
                    const float y = facesMat.at<float>(i, 1);
                    const float w = facesMat.at<float>(i, 2);
                    const float h = facesMat.at<float>(i, 3);
                    const float score = facesMat.at<float>(i, 14);
                    if (score >= 0.50f && w >= kMinFaceWidth && h >= kMinFaceHeight) {
                        int ix = std::clamp(static_cast<int>(std::round(x)), 0, frame.cols - 1);
                        int iy = std::clamp(static_cast<int>(std::round(y)), 0, frame.rows - 1);
                        int iw = std::clamp(static_cast<int>(std::round(w)), 1, frame.cols - ix);
                        int ih = std::clamp(static_cast<int>(std::round(h)), 1, frame.rows - iy);
                        faces.emplace_back(ix, iy, iw, ih);
                    }
                }
            }
        } catch (...) {
        }
    }
    if (faces.empty() && !state.detector.empty()) {
        cv::Mat gray;
        if (frame.channels() == 3) {
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        } else {
            gray = frame;
        }
        state.detector.detectMultiScale(enhanceGrayImage(gray), faces, 1.2, 5, 0, cv::Size(kMinFaceWidth, kMinFaceHeight));
    }
    if (faces.size() > 1) {
        std::sort(faces.begin(), faces.end(), [](const cv::Rect& a, const cv::Rect& b) {
            return a.area() > b.area();
        });
    }
    return faces;
}

bool hasImageExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".pgm";
}

std::filesystem::path labelsPath() {
    for (const auto& dir : runtimeSearchDirs()) {
        const auto path = dir / kLabelsFile;
        if (safeFileSize(path) > 0) {
            return path;
        }
    }
    return appOutputPath(kLabelsFile);
}

std::filesystem::path datasetPath() {
    for (const auto& dir : runtimeSearchDirs()) {
        const auto path = dir / kDatasetPath;
        if (std::filesystem::is_directory(path)) {
            try {
                auto it = std::filesystem::directory_iterator(path);
                if (it != std::filesystem::end(it)) {
                    return path;
                }
            } catch (...) {
            }
        }
    }
    return appOutputPath(kDatasetPath);
}

std::uintmax_t safeFileSize(const std::filesystem::path& path) {
    try {
        return std::filesystem::is_regular_file(path) ? std::filesystem::file_size(path) : 0;
    } catch (...) {
        return 0;
    }
}

int countSupportedDatasetImages() {
    const auto root = datasetPath();
    if (!std::filesystem::is_directory(root)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && hasImageExtension(entry.path())) {
            ++count;
        }
    }
    return count;
}

std::string startupDiagnosticsText() {
    std::ostringstream out;
    const auto labels = labelsPath();
    const auto data = datasetPath();
    out << "current_working_directory=" << cleanPath(std::filesystem::current_path()).string() << "\n";
    out << "executable_directory=" << cleanPath(gAppDir).string() << "\n";
    out << "resolved_data_root=" << cleanPath(gDataDir).string() << "\n";
    out << "dataset_path=" << data.string() << "\n";
    out << "dataset_exists=" << (std::filesystem::is_directory(data) ? "yes" : "no") << "\n";
    out << "labels_path=" << labels.string() << "\n";
    out << "labels_exists=" << (std::filesystem::is_regular_file(labels) ? "yes" : "no") << "\n";
    out << "labels_size_bytes=" << safeFileSize(labels) << "\n";
    out << "supported_dataset_images=" << countSupportedDatasetImages() << "\n";
    return out.str();
}

void writeStartupDiagnostics() {
    try {
        std::ofstream logFile(appOutputPath("startup_diagnostics.log"), std::ios::app);
        logFile << "---- startup ----\n";
        logFile << startupDiagnosticsText();
    } catch (...) {
    }
}

std::string datasetFolderCode(const std::string& folderName) {
    const auto pos = folderName.find('_');
    const std::string code = pos == std::string::npos ? folderName : folderName.substr(0, pos);
    return trim(code);
}

bool isDuplicateCopyFolder(const std::string& folderName) {
    return folderName.size() >= 7 &&
           (folderName.rfind(" - Copy") == folderName.size() - 7 ||
            folderName.find(" - Copy ") != std::string::npos);
}

void collectTrainingData(const std::map<int, Student>& students, std::vector<cv::Mat>& faces,
                         std::vector<int>& labels) {
    const auto datasetRoot = datasetPath();
    if (!std::filesystem::is_directory(datasetRoot)) {
        throw std::runtime_error("Dataset folder not found.\n\nSearched:\n" + datasetRoot.string());
    }

    const int supportedImageCount = countSupportedDatasetImages();
    if (supportedImageCount == 0) {
        throw std::runtime_error("Dataset folder contains zero supported images.\n\nSearched:\n" +
                                 datasetRoot.string() +
                                 "\n\nSupported extensions: .jpg, .jpeg, .png, .bmp");
    }

    std::map<std::string, int> labelByStudentCode;
    for (const auto& [label, student] : students) {
        labelByStudentCode[student.studentCode] = label;
    }

    std::map<int, int> countByLabel;
    for (const auto& entry : std::filesystem::directory_iterator(datasetRoot)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string folderName = entry.path().filename().string();
        if (isDuplicateCopyFolder(folderName)) {
            continue;
        }

        const auto code = datasetFolderCode(folderName);
        const auto labelIt = labelByStudentCode.find(code);
        if (labelIt == labelByStudentCode.end()) {
            continue;
        }
        const int label = labelIt->second;

        std::vector<std::filesystem::path> images;
        for (const auto& imageEntry : std::filesystem::recursive_directory_iterator(entry.path())) {
            if (imageEntry.is_regular_file() && hasImageExtension(imageEntry.path())) {
                images.push_back(imageEntry.path());
            }
        }
        std::sort(images.begin(), images.end());

        for (const auto& imagePath : images) {
            if (countByLabel[label] >= kMaxTrainingImagesPerStudent) {
                break;
            }

            cv::Mat image = cv::imread(imagePath.string(), cv::IMREAD_COLOR);
            if (image.empty()) {
                continue;
            }
            cv::Mat face = preprocessFace(image);
            if (face.empty()) {
                continue;
            }
            faces.push_back(face);
            labels.push_back(label);
            countByLabel[label] += 1;
        }
    }

    if (faces.empty()) {
        throw std::runtime_error("Dataset images were found, but zero readable training faces were collected.\n\nSearched:\n" +
                                 datasetRoot.string() +
                                 "\n\nSupported image files found: " + std::to_string(supportedImageCount));
    }
}

bool buildEmbeddingDatabaseFromDataset(const std::map<int, Student>& students) {
    const auto modelPath = findExistingFile(kEmbeddingModelFile);
    const auto datasetRoot = datasetPath();
    const auto cascadePath = findApplicationAssetFile(kCascadeFile);
    if (modelPath.empty() || !std::filesystem::is_directory(datasetRoot)) {
        return false;
    }

    cv::dnn::Net net = cv::dnn::readNetFromONNX(modelPath.string());
    if (net.empty()) {
        return false;
    }

    cv::CascadeClassifier detector;
    if (!cascadePath.empty()) {
        detector.load(cascadePath.string());
    }

    std::map<std::string, int> labelByStudentCode;
    for (const auto& [label, student] : students) {
        labelByStudentCode[student.studentCode] = label;
    }

    std::map<int, cv::Mat> sumByLabel;
    std::map<int, int> countByLabel;
    for (const auto& entry : std::filesystem::directory_iterator(datasetRoot)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string folderName = entry.path().filename().string();
        if (isDuplicateCopyFolder(folderName)) {
            continue;
        }

        const auto code = datasetFolderCode(folderName);
        const auto labelIt = labelByStudentCode.find(code);
        if (labelIt == labelByStudentCode.end()) {
            continue;
        }
        const int label = labelIt->second;

        std::vector<std::filesystem::path> images;
        for (const auto& imageEntry : std::filesystem::recursive_directory_iterator(entry.path())) {
            if (imageEntry.is_regular_file() && hasImageExtension(imageEntry.path())) {
                images.push_back(imageEntry.path());
            }
        }
        std::sort(images.begin(), images.end());

        for (const auto& imagePath : images) {
            if (countByLabel[label] >= kMaxEmbeddingImagesPerStudent) {
                break;
            }
            cv::Mat image = cv::imread(imagePath.string(), cv::IMREAD_COLOR);
            cv::Mat face = chooseFaceForEmbedding(image, detector.empty() ? nullptr : &detector);
            cv::Mat embedding = extractFaceEmbedding(net, face);
            if (embedding.empty()) {
                continue;
            }
            if (sumByLabel[label].empty()) {
                sumByLabel[label] = cv::Mat::zeros(embedding.size(), CV_32F);
            }
            sumByLabel[label] += embedding;
            countByLabel[label] += 1;
        }
    }

    std::vector<int> labels;
    cv::Mat embeddings;
    for (const auto& [label, sum] : sumByLabel) {
        if (countByLabel[label] <= 0) {
            continue;
        }
        cv::Mat average = normalizeEmbeddingRow(sum / static_cast<float>(countByLabel[label]));
        if (average.empty()) {
            continue;
        }
        labels.push_back(label);
        embeddings.push_back(average);
    }

    if (labels.empty() || embeddings.empty()) {
        return false;
    }

    const auto tempPath = appOutputPath("face_embeddings.tmp.yml.gz");
    const auto dbPath = appOutputPath(kEmbeddingDbFile);
    cv::FileStorage fs(tempPath.string(), cv::FileStorage::WRITE);
    fs << "version" << 1;
    fs << "model" << kEmbeddingModelFile;
    fs << "labels" << labels;
    fs << "embeddings" << embeddings;
    fs.release();

    if (std::filesystem::exists(dbPath)) {
        std::filesystem::remove(dbPath);
    }
    std::filesystem::rename(tempPath, dbPath);
    std::cout << "Embedding database complete: " << dbPath.string() << "\n";
    return true;
}

bool loadEmbeddingDatabase(AppState& state) {
    state.embeddingReady = false;
    state.embeddingTemplates.clear();
    const auto modelPath = findExistingFile(kEmbeddingModelFile);
    const auto dbPath = findExistingFile(kEmbeddingDbFile);
    if (modelPath.empty() || dbPath.empty()) {
        return false;
    }

    state.embeddingNet = cv::dnn::readNetFromONNX(modelPath.string());
    if (state.embeddingNet.empty()) {
        return false;
    }

    std::vector<int> labels;
    cv::Mat embeddings;
    cv::FileStorage fs(dbPath.string(), cv::FileStorage::READ);
    fs["labels"] >> labels;
    fs["embeddings"] >> embeddings;
    fs.release();
    if (labels.empty() || embeddings.empty() || embeddings.rows != static_cast<int>(labels.size())) {
        return false;
    }

    for (int i = 0; i < embeddings.rows; ++i) {
        EmbeddingTemplate item;
        item.labelId = labels[i];
        item.embedding = normalizeEmbeddingRow(embeddings.row(i));
        if (!item.embedding.empty()) {
            state.embeddingTemplates.push_back(item);
        }
    }
    state.embeddingReady = !state.embeddingTemplates.empty();
    return state.embeddingReady;
}

int trainModelFromDataset() {
    gLastTrainingError.clear();
    std::cout << "Loading labels...\n";
    try {
        const auto students = loadStudents();
        if (students.empty()) {
            gLastTrainingError = "labels.txt is missing, empty, or does not contain valid student rows.\n\nSearched:\n" +
                                 labelsPath().string();
            std::cerr << gLastTrainingError << "\n";
            return 2;
        }
        std::vector<cv::Mat> faces;
        std::vector<int> labels;

        std::cout << "Collecting training images...\n";
        collectTrainingData(students, faces, labels);

        std::cout << "Training LBPH model with " << faces.size() << " images for "
                  << std::set<int>(labels.begin(), labels.end()).size() << " students...\n";

        const auto trainerPath = appOutputPath(kTrainerFile);
        const auto tempTrainer = appOutputPath("trainer.tmp.yml.gz");
        auto model = cv::face::LBPHFaceRecognizer::create();
        model->train(faces, labels);
        model->save(tempTrainer.string());

        auto validator = cv::face::LBPHFaceRecognizer::create();
        validator->read(tempTrainer.string());

        if (std::filesystem::exists(trainerPath)) {
            std::filesystem::remove(trainerPath);
        }
        std::filesystem::rename(tempTrainer, trainerPath);

        if (buildEmbeddingDatabaseFromDataset(students)) {
            std::cout << "Embedding recognition enabled.\n";
        } else {
            std::cout << "Embedding model not found. LBPH model remains available.\n";
        }

        std::cout << "Training complete: " << trainerPath.string() << "\n";
        return 0;
    } catch (const cv::Exception& ex) {
        const auto tempTrainer = appOutputPath("trainer.tmp.yml.gz");
        if (std::filesystem::exists(tempTrainer)) {
            std::filesystem::remove(tempTrainer);
        }
        gLastTrainingError = ex.what();
        std::cerr << "Training failed: " << gLastTrainingError << "\n";
        return 3;
    } catch (const std::exception& ex) {
        const auto tempTrainer = appOutputPath("trainer.tmp.yml.gz");
        if (std::filesystem::exists(tempTrainer)) {
            std::filesystem::remove(tempTrainer);
        }
        gLastTrainingError = ex.what();
        std::cerr << "Training failed: " << gLastTrainingError << "\n";
        return 2;
    }
}

sqlite3* openDatabase() {
    sqlite3* db = nullptr;
    const auto dbPath = appOutputPath(kDbFile);
    if (sqlite3_open(dbPath.string().c_str(), &db) != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        if (db != nullptr) {
            sqlite3_close(db);
        }
        throw std::runtime_error("Could not open SQLite database: " + dbPath.string() + " - " + error);
    }
    return db;
}

void initAttendanceTable(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS school_attendance ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "student_code TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "class_name TEXT NOT NULL,"
        "section TEXT DEFAULT '',"
        "date TEXT NOT NULL,"
        "time TEXT NOT NULL,"
        "status TEXT NOT NULL DEFAULT 'Present',"
        "confidence REAL,"
        "UNIQUE(student_code, date)"
        ");";

    char* errorMessage = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
        std::string error = errorMessage ? errorMessage : "unknown error";
        sqlite3_free(errorMessage);
        throw std::runtime_error("Could not initialize attendance table: " + error);
    }
}

void loadStudentExtras(sqlite3* db, std::map<int, Student>& students) {
    const char* sql =
        "SELECT student_code, section, roll_number FROM school_students;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    std::map<std::string, Student*> byCode;
    for (auto& [label, student] : students) {
        (void)label;
        byCode[student.studentCode] = &student;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* codeText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (codeText == nullptr) {
            continue;
        }
        auto it = byCode.find(codeText);
        if (it == byCode.end()) {
            continue;
        }
        const auto* sectionText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* rollText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        it->second->section = sectionText ? sectionText : "";
        it->second->rollNumber = rollText ? rollText : "";
    }
    sqlite3_finalize(stmt);
}

bool attendanceAlreadySaved(sqlite3* db, const std::string& studentCode, const std::string& dateText) {
    const char* sql = "SELECT 1 FROM school_attendance WHERE student_code = ? AND date = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, studentCode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dateText.c_str(), -1, SQLITE_TRANSIENT);

    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

bool saveAttendance(sqlite3* db, const Student& student, double confidence, const std::string& status,
                    std::string& messageOut) {
    const std::string dateText = nowDate();
    if (attendanceAlreadySaved(db, student.studentCode, dateText)) {
        messageOut = "Already marked today: " + student.name;
        return false;
    }

    const char* sql =
        "INSERT OR IGNORE INTO school_attendance "
        "(student_code, name, class_name, section, date, time, status, confidence) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        messageOut = "Could not prepare attendance insert.";
        return false;
    }

    const std::string timeText = nowClock();
    sqlite3_bind_text(stmt, 1, student.studentCode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, student.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, student.className.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, student.section.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, dateText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, timeText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, confidence);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    if (!ok) {
        messageOut = "Could not save attendance.";
        return false;
    }

    messageOut = (status == "Late" ? "Marked late: " : "Marked: ") + student.name + " (" + student.className + ")";
    return true;
}

std::vector<AttendanceRow> loadAttendanceRows(sqlite3* db, const std::string& dateText) {
    const char* sql =
        "SELECT student_code, name, class_name, section, date, time, status "
        "FROM school_attendance WHERE date = ? "
        "AND COALESCE(status, 'Present') IN ('Present', 'Late');";
    sqlite3_stmt* stmt = nullptr;
    std::vector<AttendanceRow> rows;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_text(stmt, 1, dateText.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AttendanceRow row;
        row.studentCode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.className = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const auto* section = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        row.section = section ? section : "";
        row.date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        row.time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        row.status = status ? status : "Present";
        rows.push_back(row);
    }
    sqlite3_finalize(stmt);
    return rows;
}

void saveDailyReports(sqlite3* db, const std::map<int, Student>& students) {
    const std::string dateText = nowDate();
    const std::filesystem::path reportDir = appOutputPath(compactDateForFolder());
    std::filesystem::create_directories(reportDir);

    const auto rows = loadAttendanceRows(db, dateText);
    std::set<std::string> presentCodes;
    for (const auto& row : rows) {
        presentCodes.insert(row.studentCode);
    }

    {
        std::ofstream csv(reportDir / ("present_" + dateText + ".csv"));
        csv << "StudentCode,Name,Class,Section,Date,Time,Status\n";
        for (const auto& row : rows) {
            csv << csvEscape(row.studentCode) << ',' << csvEscape(row.name) << ','
                << csvEscape(row.className) << ',' << csvEscape(row.section) << ','
                << csvEscape(row.date) << ',' << csvEscape(row.time) << ','
                << csvEscape(row.status) << '\n';
        }
    }

    {
        std::ofstream csv(reportDir / ("absent_" + dateText + ".csv"));
        csv << "StudentCode,Name,Class,Section,RollNumber,Date,Status\n";
        for (const auto& [label, student] : students) {
            (void)label;
            if (presentCodes.count(student.studentCode) > 0) {
                continue;
            }
            csv << csvEscape(student.studentCode) << ',' << csvEscape(student.name) << ','
                << csvEscape(student.className) << ',' << csvEscape(student.section) << ','
                << csvEscape(student.rollNumber) << ',' << dateText << ",Absent\n";
        }
    }

    {
        std::ofstream text(reportDir / ("present_absent_" + dateText + ".txt"));
        text << "Attendance Report - " << dateText << "\n\n";
        text << "Present/Late: " << rows.size() << "\n";
        text << "Total Students: " << students.size() << "\n";
        text << "Absent: " << (students.size() >= presentCodes.size() ? students.size() - presentCodes.size() : 0) << "\n\n";
        text << "PRESENT / LATE\n";
        for (const auto& row : rows) {
            text << row.studentCode << " - " << row.name << " - " << row.className << " - "
                 << row.time << " - " << row.status << "\n";
        }
        text << "\nABSENT\n";
        for (const auto& [label, student] : students) {
            (void)label;
            if (presentCodes.count(student.studentCode) == 0) {
                text << student.studentCode << " - " << student.name << " - " << student.className << "\n";
            }
        }
    }
}

void drawCenteredText(cv::Mat& image, const std::string& text, int y, double scale,
                      const cv::Scalar& color, int thickness = 2) {
    int baseline = 0;
    const cv::Size size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
    const int x = std::max(10, (image.cols - size.width) / 2);
    cv::putText(image, text, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness, cv::LINE_AA);
}

void drawLabel(cv::Mat& image, const std::string& text, const cv::Point& origin, double scale,
               const cv::Scalar& color, int thickness = 1) {
    cv::putText(image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness, cv::LINE_AA);
}

void drawButton(cv::Mat& image, const cv::Rect& rect, const std::string& title, const std::string& subtitle,
                const cv::Scalar& color, const cv::Scalar& hoverColor) {
    const bool hover = pointInRect(gMouseX.load(), gMouseY.load(), rect);
    const cv::Scalar fill = hover ? hoverColor : color;
    cv::rectangle(image, rect, fill, cv::FILLED);
    cv::rectangle(image, rect, hover ? cv::Scalar(226, 232, 240) : cv::Scalar(71, 85, 105), 2);
    drawLabel(image, title, cv::Point(rect.x + 26, rect.y + 38), 0.78, cv::Scalar(248, 250, 252), 2);
    drawLabel(image, subtitle, cv::Point(rect.x + 26, rect.y + 68), 0.48, cv::Scalar(203, 213, 225), 1);
}

void drawTopButton(cv::Mat& image, const cv::Rect& rect, const std::string& text, const cv::Scalar& color) {
    const bool hover = pointInRect(gMouseX.load(), gMouseY.load(), rect);
    cv::rectangle(image, rect, hover ? cv::Scalar(color[0] + 20, color[1] + 20, color[2] + 20) : color, cv::FILLED);
    cv::rectangle(image, rect, cv::Scalar(226, 232, 240), hover ? 2 : 1);
    drawLabel(image, text, cv::Point(rect.x + 24, rect.y + 28), 0.58, cv::Scalar(248, 250, 252), 1);
}

cv::Mat buildSplashFrame(const AppState& state) {
    cv::Mat splash(kWindowHeight, kWindowWidth, CV_8UC3, cv::Scalar(5, 8, 22));
    cv::rectangle(splash, cv::Rect(20, 20, splash.cols - 40, splash.rows - 40), cv::Scalar(30, 41, 59), 2);

    if (!state.splashLogo.empty()) {
        cv::Mat logo = state.splashLogo.clone();
        const int maxWidth = splash.cols - 120;
        if (logo.cols > maxWidth) {
            const double scale = static_cast<double>(maxWidth) / static_cast<double>(logo.cols);
            cv::resize(logo, logo, cv::Size(), scale, scale, cv::INTER_AREA);
        }
        const int x = (splash.cols - logo.cols) / 2;
        const int y = 70;
        logo.copyTo(splash(cv::Rect(x, y, logo.cols, logo.rows)));
    }

    drawCenteredText(splash, "Smart Attendance System", 360, 1.1, cv::Scalar(229, 231, 235), 2);
    drawCenteredText(splash, getStatus(state), 420, 0.9, cv::Scalar(250, 204, 21), 2);

    if (state.failed) {
        drawCenteredText(splash, state.errorText, 480, 0.7, cv::Scalar(248, 113, 113), 2);
    } else {
        drawCenteredText(splash, "Please wait while existing trained files are loaded.", 480, 0.7,
                         cv::Scalar(147, 197, 253), 2);
    }

    drawCenteredText(splash, "Press Q or ESC to exit", 560, 0.7, cv::Scalar(156, 163, 175), 1);
    return splash;
}

cv::Mat buildHomeFrame(const AppState& state, const std::string& statusText) {
    cv::Mat home(kWindowHeight, kWindowWidth, CV_8UC3);
    for (int y = 0; y < home.rows; ++y) {
        const double t = static_cast<double>(y) / static_cast<double>(home.rows);
        const int b = static_cast<int>(34 + 24 * t);
        const int g = static_cast<int>(27 + 20 * t);
        const int r = static_cast<int>(18 + 10 * t);
        cv::line(home, cv::Point(0, y), cv::Point(home.cols, y), cv::Scalar(b, g, r), 1);
    }
    cv::circle(home, cv::Point(850, 90), 150, cv::Scalar(43, 79, 124), cv::FILLED);
    cv::circle(home, cv::Point(90, 585), 170, cv::Scalar(23, 74, 57), cv::FILLED);
    cv::rectangle(home, cv::Rect(42, 42, home.cols - 84, home.rows - 84), cv::Scalar(30, 41, 59), 2);

    if (!state.splashLogo.empty()) {
        cv::Mat logo = state.splashLogo.clone();
        const int maxWidth = home.cols - 190;
        if (logo.cols > maxWidth) {
            const double scale = static_cast<double>(maxWidth) / static_cast<double>(logo.cols);
            cv::resize(logo, logo, cv::Size(), scale, scale, cv::INTER_AREA);
        }
        const int x = (home.cols - logo.cols) / 2;
        logo.copyTo(home(cv::Rect(x, 50, logo.cols, logo.rows)));
    }

    drawCenteredText(home, "Smart Attendance System", 275, 1.05, cv::Scalar(248, 250, 252), 2);
    drawCenteredText(home, statusText, 318, 0.64, cv::Scalar(125, 211, 252), 2);

    drawButton(home, kHomeStartButton, "Start Attendance", "Open camera and mark present", cv::Scalar(21, 128, 61),
               cv::Scalar(22, 163, 74));
    drawButton(home, kHomeTrainButton, "Train Model", "Use dataset folder and save trainer", cv::Scalar(37, 99, 235),
               cv::Scalar(59, 130, 246));
    drawButton(home, kHomeReportsButton, "Save Reports", "Generate today's CSV and text files", cv::Scalar(180, 83, 9),
               cv::Scalar(217, 119, 6));
    drawButton(home, kHomeExitButton, "Exit App", "Close safely and save report", cv::Scalar(153, 27, 27),
               cv::Scalar(220, 38, 38));

    drawCenteredText(home, "Click a card to continue. Keyboard shortcuts still work: S, T, R, Q.", 615, 0.55,
                     cv::Scalar(203, 213, 225), 1);
    return home;
}

cv::VideoCapture openCamera() {
    cv::VideoCapture cap(0, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        cap.open(0);
    }
    if (!cap.isOpened()) {
        throw std::runtime_error("Could not open a camera.");
    }

    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 15);
    return cap;
}

void loadResources(AppState& state) {
    try {
        state.loading = true;
        state.ready = false;
        state.failed = false;
        state.errorText.clear();
        const auto logoPath = findApplicationAssetFile(kLogoFile);
        if (!logoPath.empty()) {
            state.splashLogo = cv::imread(logoPath.string(), cv::IMREAD_COLOR);
        }
        setStatus(state, "Loading student labels...");
        state.students = loadStudents();
        if (state.students.empty()) {
            throw std::runtime_error("labels.txt is missing, empty, or does not contain valid students.\n\nSearched:\n" +
                                     labelsPath().string() +
                                     "\n\nFile size bytes: " + std::to_string(safeFileSize(labelsPath())));
        }

        setStatus(state, "Loading face detector...");
        const auto cascadePath = findApplicationAssetFile(kCascadeFile);
        if (!cascadePath.empty()) {
            state.detector.load(cascadePath.string());
        }
        const auto yunetPath = findApplicationAssetFile(kYuNetModelFile);
        if (!yunetPath.empty()) {
            try {
                state.yunetDetector = cv::FaceDetectorYN::create(
                    yunetPath.string(), "", cv::Size(640, 480), 0.50f, 0.3f, 5000);
                state.yunetReady = !state.yunetDetector.empty();
            } catch (...) {
                state.yunetReady = false;
            }
        }
        if (!state.yunetReady && state.detector.empty()) {
            throw std::runtime_error("Could not load haarcascade_frontalface_default.xml or YuNet face detector.");
        }

        setStatus(state, "Loading trained model...");
        const auto trainerPath = getTrainerPath();
        if (!trainerPath.empty() && std::filesystem::is_regular_file(trainerPath)) {
            try {
                state.recognizer = cv::face::LBPHFaceRecognizer::create();
                state.recognizer->read(trainerPath);
            } catch (...) {
            }
        }
        if (loadEmbeddingDatabase(state)) {
            setStatus(state, "Deep face embedding model ready (350+ people).");
        } else if (state.recognizer.empty()) {
            throw std::runtime_error("Neither face_embeddings.yml.gz nor trainer.yml.gz could be loaded.");
        }

        state.ready = true;
        state.loading = false;
        setStatus(state, "Ready. Press S to start attendance.");
    } catch (const std::exception& ex) {
        state.failed = true;
        state.loading = false;
        state.errorText = ex.what();
        setStatus(state, "Startup failed");
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    initializeRuntimePaths(argc > 0 ? argv[0] : nullptr);

    if (argc > 1) {
        const std::string command = argv[1];
        if (command == "--train") {
            return trainModelFromDataset();
        }
        if (command == "--help" || command == "-h") {
            std::cout << "SavvyAttendanceQt\n"
                      << "  SavvyAttendanceQt.exe          Open the one-app menu\n"
                      << "  SavvyAttendanceQt.exe --train  Train trainer.yml.gz from dataset as a backup command\n";
            return 0;
        }
    }

    AppState state;
    std::thread loader(loadResources, std::ref(state));
    auto startLoader = [&]() {
        if (loader.joinable()) {
            loader.join();
        }
        loader = std::thread(loadResources, std::ref(state));
    };

    cv::namedWindow("Savvy Attendance C++", cv::WINDOW_NORMAL);
    cv::resizeWindow("Savvy Attendance C++", kWindowWidth, kWindowHeight);
    cv::setMouseCallback("Savvy Attendance C++", onMouseEvent);

    AppMode mode = AppMode::Loading;
    cv::VideoCapture camera;
    sqlite3* db = nullptr;
    bool dbReady = false;
    ArrivalSettings arrivalSettings;
    std::map<std::string, std::chrono::steady_clock::time_point> lastRecognized;
    RecognizedCandidate candidate;
    std::string statusText = "Loading face model...";
    std::thread trainingThread;
    std::atomic<bool> trainingRunning{false};
    std::atomic<int> trainingResult{-1};
    std::string lastStaticFrameKey;

    auto showStaticFrame = [&](const std::string& frameKey, const cv::Mat& frame) {
        if (frameKey == lastStaticFrameKey) {
            return;
        }
        cv::imshow("Savvy Attendance C++", frame);
        lastStaticFrameKey = frameKey;
    };

    try {
        while (true) {
            if (mode == AppMode::Loading) {
                const std::string loadingKey =
                    "loading|" + getStatus(state) + "|" + state.errorText + "|" + (state.ready ? "ready" : "not-ready") +
                    "|" + (state.failed ? "failed" : "ok");
                if (loadingKey != lastStaticFrameKey) {
                    showStaticFrame(loadingKey, buildSplashFrame(state));
                }
                const int key = cv::waitKey(30);
                const UiClickAction click = takePendingClickAction();
                if (key == 27 || key == 'q' || key == 'Q') {
                    break;
                }
                if (click == UiClickAction::ExitApp) {
                    break;
                }
                if (state.failed) {
                    continue;
                }
                if (state.ready) {
                    statusText = "Ready. Press S to start attendance.";
                    mode = AppMode::Home;
                    lastStaticFrameKey.clear();
                }
                continue;
            }

            if (mode == AppMode::Training) {
                const std::string trainingKey =
                    "training|" + getStatus(state) + "|" + std::to_string(trainingResult.load()) + "|" +
                    (trainingRunning ? "running" : "done");
                if (trainingKey != lastStaticFrameKey) {
                    showStaticFrame(trainingKey, buildSplashFrame(state));
                }
                const int key = cv::waitKey(30);
                const UiClickAction click = takePendingClickAction();
                if (key == 27 || key == 'q' || key == 'Q') {
                    break;
                }
                if (click == UiClickAction::ExitApp) {
                    break;
                }
                if (!trainingRunning) {
                    if (trainingThread.joinable()) {
                        trainingThread.join();
                    }
                    if (trainingResult == 0) {
                        statusText = "Training complete. Reloading model...";
                        setStatus(state, statusText);
                        startLoader();
                        mode = AppMode::Loading;
                        lastStaticFrameKey.clear();
                    } else {
                        statusText = "Training failed. Old model was kept unchanged.";
                        setStatus(state, statusText);
                        mode = AppMode::Home;
                        lastStaticFrameKey.clear();
                    }
                }
                continue;
            }

            if (mode == AppMode::Home) {
                const std::string homeKey =
                    "home|" + statusText + "|" + (state.ready ? "ready" : "not-ready") + "|" +
                    (state.failed ? "failed" : "ok") + "|" + std::to_string(gMouseX.load()) + "," +
                    std::to_string(gMouseY.load());
                if (homeKey != lastStaticFrameKey) {
                    showStaticFrame(homeKey, buildHomeFrame(state, statusText));
                }
                const int key = cv::waitKey(30);
                const UiClickAction click = takePendingClickAction();
                if (key == 27 || key == 'q' || key == 'Q') {
                    break;
                }
                if (click == UiClickAction::ExitApp) {
                    break;
                }
                if (key == 's' || key == 'S' || click == UiClickAction::StartAttendance) {
                    if (!state.ready) {
                        statusText = "Face model is still loading.";
                        continue;
                    }
                    try {
                        if (!dbReady) {
                            db = openDatabase();
                            initAttendanceTable(db);
                            loadStudentExtras(db, state.students);
                            arrivalSettings = loadArrivalSettings();
                            dbReady = true;
                        }
                        camera = openCamera();
                        candidate = {};
                        lastRecognized.clear();
                        statusText = "Camera ready. Look at the camera.";
                        mode = AppMode::Attendance;
                        lastStaticFrameKey.clear();
                    } catch (const std::exception& ex) {
                        statusText = ex.what();
                        lastStaticFrameKey.clear();
                    }
                    continue;
                }
                if (key == 't' || key == 'T' || click == UiClickAction::TrainModel) {
                    if (trainingThread.joinable()) {
                        trainingThread.join();
                    }
                    trainingResult = -1;
                    trainingRunning = true;
                    statusText = "Training model. Please wait...";
                    setStatus(state, statusText);
                    trainingThread = std::thread([&]() {
                        trainingResult = trainModelFromDataset();
                        trainingRunning = false;
                    });
                    mode = AppMode::Training;
                    lastStaticFrameKey.clear();
                    continue;
                }
                if (key == 'r' || key == 'R' || click == UiClickAction::SaveReports) {
                    try {
                        if (!dbReady) {
                            db = openDatabase();
                            initAttendanceTable(db);
                            loadStudentExtras(db, state.students);
                            dbReady = true;
                        }
                        saveDailyReports(db, state.students);
                        statusText = "Reports saved for today.";
                    } catch (const std::exception& ex) {
                        statusText = ex.what();
                    }
                    lastStaticFrameKey.clear();
                    continue;
                }
                continue;
            }

            cv::Mat frame;
            if (!camera.read(frame) || frame.empty()) {
                statusText = "Camera frame read failed.";
                cv::imshow("Savvy Attendance C++", buildSplashFrame(state));
                const int key = cv::waitKey(30);
                const UiClickAction click = takePendingClickAction();
                if (key == 27 || key == 'q' || key == 'Q') {
                    break;
                }
                if (click == UiClickAction::ExitApp) {
                    break;
                }
                if (key == 'h' || key == 'H' || key == 'b' || key == 'B' || click == UiClickAction::GoHome) {
                    camera.release();
                    statusText = "Ready. Press S to start attendance.";
                    mode = AppMode::Home;
                    lastStaticFrameKey.clear();
                }
                continue;
            }

            cv::Mat displayFrame;
            cv::resize(frame, displayFrame, cv::Size(kWindowWidth, kWindowHeight));

            cv::Mat gray;
            cv::cvtColor(displayFrame, gray, cv::COLOR_BGR2GRAY);

            const double meanBrightness = cv::mean(gray)[0];
            if (meanBrightness < kPoorLightMeanThreshold) {
                candidate = {};
                statusText = "Poor lighting. Add light or move closer.";
                cv::rectangle(displayFrame, cv::Rect(0, displayFrame.rows - 50, displayFrame.cols, 50),
                              cv::Scalar(5, 8, 22), cv::FILLED);
                cv::putText(displayFrame, statusText, cv::Point(20, displayFrame.rows - 18),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(21, 204, 250), 2, cv::LINE_AA);
                drawTopButton(displayFrame, kAttendanceExitButton, "Exit", cv::Scalar(127, 29, 29));
                drawTopButton(displayFrame, kAttendanceHomeButton, "Home", cv::Scalar(30, 64, 175));
                cv::imshow("Savvy Attendance C++", displayFrame);
                const int key = cv::waitKey(1);
                const UiClickAction click = takePendingClickAction();
                if (key == 27 || key == 'q' || key == 'Q') {
                    break;
                }
                if (click == UiClickAction::ExitApp) {
                    break;
                }
                if (key == 'h' || key == 'H' || key == 'b' || key == 'B' || click == UiClickAction::GoHome) {
                    camera.release();
                    statusText = "Ready. Press S to start attendance.";
                    mode = AppMode::Home;
                    lastStaticFrameKey.clear();
                }
                continue;
            }

            cv::Mat detectGray = enhanceGrayImage(gray);

            std::vector<cv::Rect> faces;
            state.detector.detectMultiScale(detectGray, faces, 1.2, 5, 0, cv::Size(60, 60));

            if (!faces.empty()) {
                std::sort(faces.begin(), faces.end(), [](const cv::Rect& a, const cv::Rect& b) {
                    return a.area() > b.area();
                });

                const cv::Rect faceRect = faces.front();
                cv::rectangle(displayFrame, faceRect, cv::Scalar(0, 255, 255), 2);

                if (faceRect.width >= kMinFaceWidth && faceRect.height >= kMinFaceHeight) {
                    cv::Mat rawFace = cropFaceWithPadding(gray, faceRect);
                    cv::Mat face = preprocessFace(rawFace);

                    if (!face.empty()) {
                        int label = -1;
                        double confidence = 9999.0;
                        bool recognized = false;
                        if (const auto embeddingMatch = recognizeByEmbedding(state, rawFace)) {
                            label = embeddingMatch->first;
                            confidence = (1.0 - embeddingMatch->second) * 100.0;
                            recognized = state.students.count(label) > 0;
                        } else {
                            state.recognizer->predict(face, label, confidence);
                            recognized = confidence < kConfidenceThreshold && state.students.count(label) > 0;
                        }

                        if (recognized) {
                            const auto& student = state.students.at(label);
                            const auto now = std::chrono::steady_clock::now();

                            const bool sameCandidate =
                                candidate.labelId == label &&
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

                            cv::putText(displayFrame, student.name + "  " + std::to_string(static_cast<int>(confidence)),
                                        cv::Point(faceRect.x, std::max(30, faceRect.y - 10)),
                                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);

                            const auto lastIt = lastRecognized.find(student.studentCode);
                            const bool cooldownPassed =
                                lastIt == lastRecognized.end() ||
                                std::chrono::duration_cast<std::chrono::seconds>(now - lastIt->second).count() >= kCooldownSeconds;

                            if (candidate.count >= kConfirmFrames && cooldownPassed) {
                                std::string saveMessage;
                                const std::string markStatus = attendanceStatusForTime(student, nowClock(), arrivalSettings);
                                if (saveAttendance(db, student, candidate.bestConfidence, markStatus, saveMessage)) {
                                    statusText = saveMessage;
                                    lastRecognized[student.studentCode] = now;
                                } else if (!saveMessage.empty()) {
                                    statusText = saveMessage;
                                }
                                candidate = {};
                            }
                        } else {
                            candidate = {};
                            cv::putText(displayFrame, "Unknown", cv::Point(faceRect.x, std::max(30, faceRect.y - 10)),
                                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
                        }
                    }
                } else {
                    candidate = {};
                    cv::putText(displayFrame, "Move closer", cv::Point(faceRect.x, std::max(30, faceRect.y - 10)),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
                }
            } else {
                candidate = {};
            }

            cv::rectangle(displayFrame, cv::Rect(0, displayFrame.rows - 50, displayFrame.cols, 50),
                          cv::Scalar(5, 8, 22), cv::FILLED);
            cv::putText(displayFrame, statusText, cv::Point(20, displayFrame.rows - 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(74, 222, 128), 2, cv::LINE_AA);
            drawTopButton(displayFrame, kAttendanceExitButton, "Exit", cv::Scalar(127, 29, 29));
            drawTopButton(displayFrame, kAttendanceHomeButton, "Home", cv::Scalar(30, 64, 175));
            cv::putText(displayFrame, "Reports save on exit", cv::Point(20, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(229, 231, 235), 1, cv::LINE_AA);

            cv::imshow("Savvy Attendance C++", displayFrame);
            const int key = cv::waitKey(1);
            const UiClickAction click = takePendingClickAction();
            if (key == 27 || key == 'q' || key == 'Q') {
                break;
            }
            if (click == UiClickAction::ExitApp) {
                break;
            }
            if (key == 'h' || key == 'H' || key == 'b' || key == 'B' || click == UiClickAction::GoHome) {
                camera.release();
                statusText = "Ready. Press S to start attendance.";
                mode = AppMode::Home;
                lastStaticFrameKey.clear();
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Runtime error: " << ex.what() << '\n';
    }

    if (camera.isOpened()) {
        camera.release();
    }
    if (db != nullptr) {
        try {
            saveDailyReports(db, state.students);
        } catch (const std::exception& ex) {
            std::cerr << "Report save error: " << ex.what() << '\n';
        }
        sqlite3_close(db);
    }
    if (loader.joinable()) {
        loader.join();
    }
    if (trainingThread.joinable()) {
        trainingThread.join();
    }
    cv::destroyAllWindows();
    return 0;
}
