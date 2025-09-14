#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <stdexcept>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <memory>
#include <sstream> // For std::ostringstream
#include <thread>  // For std::this_thread

// Windows & DirectX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ONNX Runtime
#include <onnxruntime_cxx_api.h>
#pragma comment(lib, "onnxruntime.lib")

// OpenCV
#include <opencv2/opencv.hpp>
#ifdef _DEBUG
#pragma comment(lib, "opencv_world4110d.lib") // ��������OpenCV�汾�޸�
#else
#pragma comment(lib, "opencv_world4110.lib") // ��������OpenCV�汾�޸�
#endif

// Input Simulator (ȷ��ͷ�ļ�����Ŀ·����)
#include "InputSimulator.hpp"

// Forward Declarations
struct Detection;
struct TimingDetails;
class Config;
class ScreenCapturer;
class ObjectDetector;
class MouseController;
class AimAssistant;

// --- ������ ---
template<class T>
void SafeRelease(T** ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

// --- ���ݽṹ���� ---

struct Detection {
    cv::Rect box;
    float confidence;
    int class_id;
};

struct TimingDetails {
    double preprocess_ms = 0;
    double inference_ms = 0;
    double postprocess_ms = 0;
    double total_loop_ms = 0;
};

// --- 1. �������� (Config) ---
class Config {
public:
    const wchar_t* model_path = L"apex_final.onnx";
    const bool use_end_to_end_onnx = true;
    const char* trt_cache_path = ".\\engine_cache";
    const int crop_size = 640;
    const int max_lock_distance_pixels = 100;
    const float confidence_threshold = 0.5f;
    const float nms_threshold = 0.4f;
    const int smooth_aim_key = VK_LBUTTON;
    const int single_shot_key = VK_F8;
    const double aim_smoothing = 1.0;
    const double target_y_ratio = 0.3;
    const double sensitivity = 1.0;
    const double pixels_for_360_turn = 16410;
    const double horizontal_fov = 120.0;
    const bool enable_visualization = true;
    const std::string window_name = "YOLO Real-time Detection";
};

// --- 2. �������� (MouseController) ---
class MouseController {
public:
    MouseController() {
        std::cout << "--- Initializing Mouse Simulation Driver... ---" << std::endl;
        hMouseDll = LoadLibrary(L"IbInputSimulator.dll");
        if (!hMouseDll) {
            throw std::runtime_error("Failed to load IbInputSimulator.dll! Ensure it's in the EXE directory.");
        }

        IbSendInit_ptr = (pIbSendInit)GetProcAddress(hMouseDll, "IbSendInit");
        IbSendDestroy_ptr = (pIbSendDestroy)GetProcAddress(hMouseDll, "IbSendDestroy");
        IbSendMouseMove_ptr = (pIbSendMouseMove)GetProcAddress(hMouseDll, "IbSendMouseMove");

        if (!IbSendInit_ptr || !IbSendDestroy_ptr || !IbSendMouseMove_ptr) {
            FreeLibrary(hMouseDll);
            hMouseDll = nullptr;
            throw std::runtime_error("Failed to find required functions in IbInputSimulator.dll.");
        }

        Send::Error error = IbSendInit_ptr(Send::SendType::Razer, 0, nullptr);
        if (error != Send::Error::Success) {
            FreeLibrary(hMouseDll);
            hMouseDll = nullptr;
            throw std::runtime_error("Failed to initialize mouse driver! Code: " + std::to_string(static_cast<int>(error)) + ". Run as Administrator.");
        }
        std::cout << "--- Mouse driver initialized successfully! ---" << std::endl;
    }

    ~MouseController() {
        if (hMouseDll) {
            if (IbSendDestroy_ptr) IbSendDestroy_ptr();
            FreeLibrary(hMouseDll);
            std::cout << "--- Mouse driver resources cleaned up. ---" << std::endl;
        }
    }

    MouseController(const MouseController&) = delete;
    MouseController& operator=(const MouseController&) = delete;

    void MoveRelative(int dx, int dy) {
        if (IbSendMouseMove_ptr) {
            IbSendMouseMove_ptr(dx, dy, Send::MoveMode::Relative);
        }
    }

private:
    typedef Send::Error(__stdcall* pIbSendInit)(Send::SendType, Send::InitFlags, void*);
    typedef void(__stdcall* pIbSendDestroy)();
    typedef bool(__stdcall* pIbSendMouseMove)(int, int, Send::MoveMode);

    HMODULE hMouseDll = nullptr;
    pIbSendInit IbSendInit_ptr = nullptr;
    pIbSendDestroy IbSendDestroy_ptr = nullptr;
    pIbSendMouseMove IbSendMouseMove_ptr = nullptr;
};

// --- 3. ��Ļ��׽�� (ScreenCapturer) - �Ż��汾 ---
class ScreenCapturer {
public:
    ScreenCapturer(int crop_width, int crop_height) {
        std::cout << "--- Initializing D3D for screen capture... ---" << std::endl;
        HRESULT hr;

        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
        if (FAILED(hr)) throw std::runtime_error("Failed to create DXGI Factory.");
        if (FAILED(pFactory->EnumAdapters1(0, &pAdapter))) throw std::runtime_error("Failed to enumerate adapters.");
        if (FAILED(pAdapter->EnumOutputs(0, &pOutput))) throw std::runtime_error("Failed to enumerate outputs.");

        DXGI_OUTPUT_DESC outputDesc;
        pOutput->GetDesc(&outputDesc);
        width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

        if (FAILED(D3D11CreateDevice(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice, nullptr, &pContext))) {
            throw std::runtime_error("Failed to create D3D11 device.");
        }
        if (FAILED(pOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pOutput1))) {
            throw std::runtime_error("Failed to query IDXGIOutput1.");
        }
        if (FAILED(pOutput1->DuplicateOutput(pDevice, &pDuplicator))) {
            throw std::runtime_error("Failed to create output duplication.");
        }

        D3D11_TEXTURE2D_DESC desc;
        desc.Width = crop_width;
        desc.Height = crop_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        hr = pDevice->CreateTexture2D(&desc, NULL, &m_pStagingTexture);
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create staging texture for cropping.");
        }
        std::cout << "--- Screen capture initialized successfully (" << width << "x" << height << ") ---" << std::endl;
    }

    ~ScreenCapturer() {
        std::cout << "--- Cleaning up D3D resources... ---" << std::endl;
        SafeRelease(&m_pStagingTexture);
        SafeRelease(&pDuplicator);
        SafeRelease(&pOutput1);
        SafeRelease(&pOutput);
        SafeRelease(&pAdapter);
        SafeRelease(&pFactory);
        SafeRelease(&pContext);
        SafeRelease(&pDevice);
    }

    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    bool CaptureFrame(cv::Mat& frame, const cv::Rect& crop_region) {
        IDXGIResource* pDesktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        HRESULT hr = pDuplicator->AcquireNextFrame(16, &frameInfo, &pDesktopResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) {
            pDuplicator->ReleaseFrame();
            std::cerr << "AcquireNextFrame failed. HRESULT: 0x" << std::hex << hr << std::endl;
            return false;
        }

        ID3D11Texture2D* pAcquiredDesktopImage = nullptr;
        hr = pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pAcquiredDesktopImage);
        SafeRelease(&pDesktopResource);
        if (FAILED(hr)) {
            pDuplicator->ReleaseFrame();
            return false;
        }

        D3D11_BOX sourceRegion;
        sourceRegion.left = crop_region.x;
        sourceRegion.right = crop_region.x + crop_region.width;
        sourceRegion.top = crop_region.y;
        sourceRegion.bottom = crop_region.y + crop_region.height;
        sourceRegion.front = 0;
        sourceRegion.back = 1;

        pContext->CopySubresourceRegion(m_pStagingTexture, 0, 0, 0, 0, pAcquiredDesktopImage, 0, &sourceRegion);

        D3D11_MAPPED_SUBRESOURCE mappedResource;
        hr = pContext->Map(m_pStagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (FAILED(hr)) {
            SafeRelease(&pAcquiredDesktopImage);
            pDuplicator->ReleaseFrame();
            return false;
        }

        cv::Mat bgra_frame(crop_region.height, crop_region.width, CV_8UC4, mappedResource.pData, mappedResource.RowPitch);
        cv::cvtColor(bgra_frame, frame, cv::COLOR_BGRA2BGR);

        pContext->Unmap(m_pStagingTexture, 0);
        SafeRelease(&pAcquiredDesktopImage);
        pDuplicator->ReleaseFrame();
        return true;
    }

    int getWidth() const { return width; }
    int getHeight() const { return height; }

private:
    ID3D11Texture2D* m_pStagingTexture = nullptr;
    IDXGIFactory1* pFactory = nullptr;
    IDXGIAdapter1* pAdapter = nullptr;
    IDXGIOutput* pOutput = nullptr;
    IDXGIOutput1* pOutput1 = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    IDXGIOutputDuplication* pDuplicator = nullptr;
    int width = 0;
    int height = 0;
};

// --- 4. Ŀ������ (ObjectDetector) ---
class ObjectDetector {
public:
    ObjectDetector(const Config& cfg)
        : env(ORT_LOGGING_LEVEL_WARNING, "Realtime_YOLO_Detector"), session(nullptr) {

        std::cout << "--- Initializing ONNX Runtime and YOLO model... ---" << std::endl;
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        OrtTensorRTProviderOptions trt_options{};
        trt_options.device_id = 0;
        trt_options.trt_fp16_enable = 1;
        trt_options.trt_engine_cache_enable = 1;
        trt_options.trt_engine_cache_path = cfg.trt_cache_path;
        session_options.AppendExecutionProvider_TensorRT(trt_options);

        session = Ort::Session(env, cfg.model_path, session_options);
        std::cout << "--- Model loaded successfully! ---" << std::endl;
    }

    ObjectDetector(const ObjectDetector&) = delete;
    ObjectDetector& operator=(const ObjectDetector&) = delete;

    void Detect(const cv::Mat& image, std::vector<Detection>& detections, const Config& cfg, TimingDetails& timings) {
        detections.clear();
        auto stage_start = std::chrono::high_resolution_clock::now();

        auto input_tensor_info = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto& input_shape = input_tensor_info.GetShape();
        const int64_t input_height = input_shape[2];
        const int64_t input_width = input_shape[3];

        float ratio_h = static_cast<float>(input_height) / image.rows;
        float ratio_w = static_cast<float>(input_width) / image.cols;
        float ratio = std::min(ratio_h, ratio_w);
        int new_w = static_cast<int>(image.cols * ratio);
        int new_h = static_cast<int>(image.rows * ratio);

        cv::Mat resized_img;
        cv::resize(image, resized_img, cv::Size(new_w, new_h));

        cv::Mat canvas = cv::Mat::ones(cv::Size(static_cast<int>(input_width), static_cast<int>(input_height)), CV_8UC3) * 114;
        int paste_x = (static_cast<int>(input_width) - new_w) / 2;
        int paste_y = (static_cast<int>(input_height) - new_h) / 2;
        resized_img.copyTo(canvas(cv::Rect(paste_x, paste_y, new_w, new_h)));

        cv::Mat blob = cv::dnn::blobFromImage(canvas, 1.0 / 255.0, cv::Size(input_width, input_height), cv::Scalar(), true, false);

        auto stage_end_preprocess = std::chrono::high_resolution_clock::now();
        timings.preprocess_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_preprocess - stage_start).count() / 1000.0;

        Ort::AllocatorWithDefaultOptions allocator;
        auto input_name_ptr = session.GetInputNameAllocated(0, allocator);
        auto output_name_ptr = session.GetOutputNameAllocated(0, allocator);
        const char* input_names[] = { input_name_ptr.get() };
        const char* output_names[] = { output_name_ptr.get() };

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, blob.ptr<float>(), blob.total(), input_shape.data(), input_shape.size());

        auto output_tensors = session.Run(Ort::RunOptions{ nullptr }, input_names, &input_tensor, 1, output_names, 1);
        auto stage_end_inference = std::chrono::high_resolution_clock::now();
        timings.inference_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_inference - stage_end_preprocess).count() / 1000.0;

        if (cfg.use_end_to_end_onnx) {
            const float* output_data = output_tensors.front().GetTensorData<float>();
            const auto& output_shape = output_tensors.front().GetTensorTypeAndShapeInfo().GetShape();
            const int num_detections = static_cast<int>(output_shape[1]);

            for (int i = 0; i < num_detections; ++i) {
                const float confidence = output_data[i * 6 + 4];
                if (confidence >= cfg.confidence_threshold) {
                    const float x1 = output_data[i * 6 + 0];
                    const float y1 = output_data[i * 6 + 1];
                    const float x2 = output_data[i * 6 + 2];
                    const float y2 = output_data[i * 6 + 3];
                    const int class_id = static_cast<int>(output_data[i * 6 + 5]);
                    int left = static_cast<int>((x1 - paste_x) / ratio);
                    int top = static_cast<int>((y1 - paste_y) / ratio);
                    int width = static_cast<int>((x2 - x1) / ratio);
                    int height = static_cast<int>((y2 - y1) / ratio);
                    detections.emplace_back(Detection{ cv::Rect(left, top, width, height), confidence, class_id });
                }
            }
        }
        else {
            const float* output_data = output_tensors.front().GetTensorData<float>();
            const auto& output_shape = output_tensors.front().GetTensorTypeAndShapeInfo().GetShape();
            cv::Mat output_mat(output_shape[1], output_shape[2], CV_32F, (void*)output_data);
            output_mat = output_mat.t();

            std::vector<cv::Rect> boxes;
            std::vector<float> confidences;
            std::vector<int> class_ids;
            for (int i = 0; i < output_mat.rows; ++i) {
                cv::Mat classes_scores = output_mat.row(i).colRange(4, output_mat.cols);
                cv::Point class_id_point;
                double max_score;
                cv::minMaxLoc(classes_scores, 0, &max_score, 0, &class_id_point);
                if (max_score > cfg.confidence_threshold) {
                    confidences.push_back(static_cast<float>(max_score));
                    class_ids.push_back(class_id_point.x);
                    float cx = output_mat.at<float>(i, 0);
                    float cy = output_mat.at<float>(i, 1);
                    float w = output_mat.at<float>(i, 2);
                    float h = output_mat.at<float>(i, 3);
                    int left = static_cast<int>((cx - 0.5f * w - paste_x) / ratio);
                    int top = static_cast<int>((cy - 0.5f * h - paste_y) / ratio);
                    int width = static_cast<int>(w / ratio);
                    int height = static_cast<int>(h / ratio);
                    boxes.emplace_back(left, top, width, height);
                }
            }
            std::vector<int> nms_indices;
            cv::dnn::NMSBoxes(boxes, confidences, cfg.confidence_threshold, cfg.nms_threshold, nms_indices);
            for (int idx : nms_indices) {
                detections.emplace_back(Detection{ boxes[idx], confidences[idx], class_ids[idx] });
            }
        }
        auto stage_end_postprocess = std::chrono::high_resolution_clock::now();
        timings.postprocess_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_postprocess - stage_end_inference).count() / 1000.0;
    }

private:
    Ort::Env env;
    Ort::Session session;
};

// --- 5. ����Э���� (AimAssistant) ---
class AimAssistant {
public:
    AimAssistant()
        : cfg(),
        capturer(cfg.crop_size, cfg.crop_size), // ���� crop_size
        detector(cfg),
        mouse(),
        crop_region((capturer.getWidth() - cfg.crop_size) / 2, (capturer.getHeight() - cfg.crop_size) / 2, cfg.crop_size, cfg.crop_size),
        crop_center(cfg.crop_size / 2, cfg.crop_size / 2),
        is_visualizing(cfg.enable_visualization)
    {
        if (capturer.getWidth() < cfg.crop_size || capturer.getHeight() < cfg.crop_size) {
            throw std::runtime_error("Screen resolution is smaller than configured crop_size.");
        }
        mouse_correction_factor = (cfg.horizontal_fov / static_cast<double>(capturer.getWidth()))
            * (cfg.pixels_for_360_turn / 360.0);
        if (is_visualizing) {
            cv::namedWindow(cfg.window_name, cv::WINDOW_AUTOSIZE);
        }
        std::cout << "--- Detection will run on a centered " << cfg.crop_size << "x" << cfg.crop_size << " region. ---" << std::endl;
        print_instructions();
    }

    ~AimAssistant() {
        if (is_visualizing) cv::destroyAllWindows();
    }

    void Run() {
        while (true) {
            auto loop_start_time = std::chrono::high_resolution_clock::now();
            if (!capturer.CaptureFrame(captured_frame, crop_region)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            detector.Detect(captured_frame, detections, cfg, timings);
            Detection* best_target = findBestTarget();
            handleMouseInput(best_target);
            auto loop_end_time = std::chrono::high_resolution_clock::now();
            timings.total_loop_ms = std::chrono::duration<double, std::milli>(loop_end_time - loop_start_time).count();
            updateAndPrintStats();
            if (is_visualizing) {
                handleVisualization(best_target);
            }
            char key = static_cast<char>(cv::waitKey(1));
            if (key == 27) { // ESC
                std::cout << "\nESC pressed. Exiting..." << std::endl;
                break;
            }
            if (key == 'v' || key == 'V') {
                toggleVisualization();
            }
        }
    }

private:
    void print_instructions() {
        std::cout << "\n--- Controls ---" << std::endl;
        std::cout << "    [Hold Left Mouse Button] to smoothly lock onto a target." << std::endl;
        std::cout << "    [Press F8] to move mouse to target ONCE (for accuracy test)." << std::endl;
        std::cout << "    [Press V in visualization window] to toggle visualization ON/OFF." << std::endl;
        std::cout << "    [Press ESC in visualization window] to quit." << std::endl;
    }

    Detection* findBestTarget() {
        Detection* target = nullptr;
        double min_dist_to_center = cfg.max_lock_distance_pixels;
        for (auto& det : detections) {
            cv::Point box_center(det.box.x + det.box.width / 2, det.box.y + det.box.height / 2);
            double dist = std::hypot(box_center.x - crop_center.x, box_center.y - crop_center.y);
            if (dist < min_dist_to_center) {
                min_dist_to_center = dist;
                target = &det;
            }
        }
        return target;
    }

    void handleMouseInput(const Detection* target) {
        if (!target) return;
        if (GetAsyncKeyState(cfg.smooth_aim_key) & 0x8000) {
            cv::Point target_point(
                target->box.x + target->box.width / 2,
                target->box.y + static_cast<int>(target->box.height * cfg.target_y_ratio)
            );
            double dx_pixels = target_point.x - crop_center.x;
            double dy_pixels = target_point.y - crop_center.y;
            double corrected_dx = dx_pixels * mouse_correction_factor;
            double corrected_dy = dy_pixels * mouse_correction_factor;
            int move_x = static_cast<int>(corrected_dx * cfg.aim_smoothing);
            int move_y = static_cast<int>(corrected_dy * cfg.aim_smoothing);
            if (std::abs(move_x) > 0 || std::abs(move_y) > 0) {
                mouse.MoveRelative(move_x, move_y);
            }
        }
        static bool f8_was_pressed = false;
        bool f8_is_pressed = GetAsyncKeyState(cfg.single_shot_key) & 0x8000;
        if (f8_is_pressed && !f8_was_pressed) {
            cv::Point target_point(
                target->box.x + target->box.width / 2,
                target->box.y + static_cast<int>(target->box.height * cfg.target_y_ratio)
            );
            double dx_pixels = target_point.x - crop_center.x;
            double dy_pixels = target_point.y - crop_center.y;
            double corrected_dx = dx_pixels * mouse_correction_factor;
            double corrected_dy = dy_pixels * mouse_correction_factor;
            int move_x = static_cast<int>(corrected_dx);
            int move_y = static_cast<int>(corrected_dy);
            if (std::abs(move_x) > 0 || std::abs(move_y) > 0) {
                mouse.MoveRelative(move_x, move_y);
            }
        }
        f8_was_pressed = f8_is_pressed;
    }

    void updateAndPrintStats() {
        const double smoothing_factor = 0.05;
        if (is_first_frame) {
            smoothed_total = timings.total_loop_ms;
            smoothed_pre = timings.preprocess_ms;
            smoothed_inf = timings.inference_ms;
            smoothed_post = timings.postprocess_ms;
            is_first_frame = false;
        }
        else {
            smoothed_total = smoothing_factor * timings.total_loop_ms + (1.0 - smoothing_factor) * smoothed_total;
            smoothed_pre = smoothing_factor * timings.preprocess_ms + (1.0 - smoothing_factor) * smoothed_pre;
            smoothed_inf = smoothing_factor * timings.inference_ms + (1.0 - smoothing_factor) * smoothed_inf;
            smoothed_post = smoothing_factor * timings.postprocess_ms + (1.0 - smoothing_factor) * smoothed_post;
        }
        if (++frame_count_for_console % 30 == 0 && smoothed_total > 0) {
            double smoothed_fps = 1000.0 / smoothed_total;
            std::cout << std::fixed << std::setprecision(1)
                << "\r[LIVE] FPS: " << std::setw(5) << smoothed_fps
                << " | Total Delay: " << std::setw(5) << smoothed_total << "ms"
                << " (Pre: " << smoothed_pre << ", Inf: " << smoothed_inf << ", Post: " << smoothed_post << ")"
                << "        " << std::flush;
        }
    }

    void handleVisualization(const Detection* best_target) {
        cv::circle(captured_frame, crop_center, static_cast<int>(cfg.max_lock_distance_pixels), cv::Scalar(255, 255, 0), 1);
        for (const auto& det : detections) {
            cv::Scalar color = (&det == best_target) ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
            cv::rectangle(captured_frame, det.box, color, 2);
            std::string label = "ID:" + std::to_string(det.class_id) + " " + cv::format("%.2f", det.confidence);
            cv::putText(captured_frame, label, cv::Point(det.box.x, det.box.y - 5), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
        }
        if (smoothed_total > 0) {
            double smoothed_fps = 1000.0 / smoothed_total;
            std::ostringstream stats_stream;
            stats_stream << std::fixed << std::setprecision(1) << "FPS: " << smoothed_fps;
            cv::putText(captured_frame, stats_stream.str(), cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 255, 255), 2);
        }
        cv::imshow(cfg.window_name, captured_frame);
    }

    void toggleVisualization() {
        is_visualizing = !is_visualizing;
        if (is_visualizing) {
            cv::namedWindow(cfg.window_name, cv::WINDOW_AUTOSIZE);
            std::cout << "\n[INFO] Visualization toggled ON." << std::endl;
        }
        else {
            cv::destroyAllWindows();
            std::cout << "\n[INFO] Visualization toggled OFF. Console stats will continue." << std::endl;
        }
    }

    Config cfg;
    ScreenCapturer capturer;
    ObjectDetector detector;
    MouseController mouse;
    const cv::Rect crop_region;
    const cv::Point crop_center;
    double mouse_correction_factor = 1.0;
    cv::Mat captured_frame;
    std::vector<Detection> detections;
    TimingDetails timings;
    bool is_visualizing;
    double smoothed_total = 0.0, smoothed_pre = 0.0, smoothed_inf = 0.0, smoothed_post = 0.0;
    bool is_first_frame = true;
    int frame_count_for_console = 0;
};

// --- ������ ---
int main() {
    try {
        AimAssistant assistant;
        assistant.Run();
    }
    catch (const std::exception& e) {
        std::cerr << "\n\n[FATAL ERROR] An unrecoverable error occurred: " << e.what() << std::endl;
        std::cerr << "Please check the following:\n"
            << "1. The ONNX model file is in the correct directory.\n"
            << "2. Required DLLs (onnxruntime.dll, opencv_world*.dll, IbInputSimulator.dll) are present.\n"
            << "3. You have the correct NVIDIA drivers and CUDA/cuDNN installed for TensorRT support.\n"
            << "4. Run the program as an Administrator.\n\n"
            << "Press Enter to exit." << std::endl;
        std::cin.get();
        return -1;
    }
    std::cout << "\nProgram finished successfully." << std::endl;
    return 0;
}