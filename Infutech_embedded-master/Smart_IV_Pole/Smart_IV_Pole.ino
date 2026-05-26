

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_system.h"
#include "esp_task_wdt.h"    // 하드웨어 워치독
#include "ads1232.h"
#include "cnn_detector.h"
#include "ble_wifi_prov.h"   // 커스텀 BLE WiFi 프로비저닝 (Flutter 앱용)

// ── 펌웨어 버전 ───────────────────────────────────────────────────
#define FW_VERSION  "1.3.0"

// ── 워치독 ────────────────────────────────────────────────────────
#define WDT_TIMEOUT_SEC  30

// ── 핀 정의 ──────────────────────────────────────────────────────
#define ADS_DOUT  19
#define ADS_SCLK  18
#define ADS_PDWN  23
#define ADS_GAIN0 33
#define ADS_GAIN1 32

// ── HTTP REST API 설정 ────────────────────────────────────────────
#define SERVER_URL    "http://54.243.192.21"          // FastAPI 서버 주소 (포트 80)
// 서버 없이 개발/데이터수집 중이면 false → HTTP 전송 시도 안 함 (로그 깔끔).
// 시리얼 'http on'/'http off' 로 런타임 토글 가능.
bool httpEnabled = false;
// 기기 인증 키 — 백엔드 get_device_from_headers 가 X-Device-Key 헤더를 필수로 요구.
// 백엔드 Device.device_key 가 비어있으면 아무 값이나 통과(헤더 존재만 필요).
// 등록 시 키가 지정됐다면 그 값으로 교체할 것.
#define DEVICE_KEY    ""

// ── 타이밍 설정 ───────────────────────────────────────────────────
#define WEIGHT_MS         4000     // 무게 측정 주기 (4초) — 방울 이산성 평균화
#define HTTP_SEND_MS      5000     // 측정 데이터 서버 전송 주기 (5초)
#define HEARTBEAT_MS      60000    // 하트비트 주기 (60초)
#define BOOT_DELAY_MS     2000UL   // 전원 ON 후 영점까지 대기

// ── 기준 유속 측정 (baseline) ─────────────────────────────────────
// gtt/drip factor 입력 없이, 초기 일정 시간 동안의 평균 유속을 측정해
// 그 값을 "정상 기준선"으로 고정한다. (자가 기준선, self-baseline)
#define BASELINE_DURATION_MS  60000UL   // 기준 유속 측정 시간 (60초)
// sanity check 범위 (g/s): 이 범위 벗어난 baseline 은 거부하고 재측정
#define BASELINE_MIN_FLOW   0.005f   // 이 미만이면 막힘 상태 → baseline 거부
#define BASELINE_MAX_FLOW   1.000f   // 이 초과면 비현실적 과속 → baseline 거부

// ── 수액 자동 감지 임계값 ─────────────────────────────────────────
#define WEIGHT_HANG_G    100.0f   // 수액 감지: 이 이상이면 수액 걸린 것으로 판단
#define WEIGHT_REMOVE_G  80.0f   // 수액 제거: 이 미만이면 수액 없는 것으로 판단

// ── 수액 안정화 (흔들림 잡기) ────────────────────────────────────
#define STABILIZE_WINDOW    5       // 안정화 판정에 사용할 최근 측정 샘플 수
#define STABILIZE_RANGE_G   0.5f    // 연속 차분(변화율)의 변동폭 < 이 값이면 안정
#define STABILIZE_TIMEOUT_MS  60000UL  // 60초 안에 안정 안 되면 강제 진입

// ── 로드셀 교정 계수 ─────────────────────────────────────────────
#define CALIB_FACTOR_DEFAULT  1689.1994f   // 재교정: 실제390g/표시401g 보정 (1642.8623×401/390)

// ── EMA 안정화 샘플 수 ────────────────────────────────────────────
#define WARMUP_SAMPLES  2    // 측정 준비(첫 prevWeight 세팅) — EMA 미사용이라 짧게

// =================================================================
// 기기 고유 ID — MAC 주소 뒤 6자리 기반 ("IVPOLE_AABBCC")
// QR 코드 라벨에 인쇄 → 앱이 스캔하여 백엔드에서 기기 특정
// =================================================================
char deviceId[20];     // "INFUCARE_A1B2"
char bleName[24];      // BLE 검색명 = deviceId


// =================================================================
// 부팅 절차 단계 정의
// =================================================================
enum IVPhase {
  PHASE_BOOT,        // 전원 ON — 2초 대기 중
  PHASE_TARE,        // 자동 영점 조정 중
  PHASE_WAIT,        // 수액 감지 대기
  PHASE_STABILIZE,   // 수액 흔들림 잡힐 때까지 대기 (교정 직전)
  PHASE_BASELINE,    // 기준 유속 측정 중 (60초)
  PHASE_WARMUP,      // EMA 안정화 대기 (8샘플)
  PHASE_MONITOR,     // 주입 모니터링 중
  PHASE_DONE         // 주입 완료
};

// ─────────────────────────────────────────────────────────────────
// 전역 객체
// ─────────────────────────────────────────────────────────────────
ADS1232      loadCell(ADS_DOUT, ADS_SCLK, ADS_PDWN, ADS_GAIN0, ADS_GAIN1);
CNNDetector  detector;
BLEWiFiProv  bleProv;   // BLE 기반 WiFi 프로비저닝 (Flutter 앱)

// ─────────────────────────────────────────────────────────────────
// IV 상태 구조체
// ─────────────────────────────────────────────────────────────────
struct IVState {
  float baselineFlowRate = 0;   // 초기 측정한 기준 유속 (g/s), 정상의 기준점
  float finishWeight     = 0;
  float currentWeight    = 0;
  float prevWeight       = 0;
  float currentFlowRate  = 0;
  bool  started          = false;
  bool  complete         = false;
  int   warmup           = 0;
} iv;

// ─────────────────────────────────────────────────────────────────
// 부팅 절차 상태 변수
// ─────────────────────────────────────────────────────────────────
IVPhase       ivPhase            = PHASE_BOOT;
float         baselineWeightStart = 0;     // 기준 유속 측정 시작 무게 (g)
unsigned long baselineStartMs     = 0;     // 기준 유속 측정 시작 시각
unsigned long bootMs              = 0;

// 안정화 단계 — 최근 N개 측정값의 변동 추적
float         stabilizeBuf[STABILIZE_WINDOW] = {0};
int           stabilizeIdx     = 0;
int           stabilizeCount   = 0;
unsigned long stabilizeStartMs = 0;

// ─────────────────────────────────────────────────────────────────
// 타이밍 변수
// ─────────────────────────────────────────────────────────────────
unsigned long lastWeightMs = 0;
unsigned long lastHttpSendMs  = 0;
unsigned long lastHeartbeatMs = 0;

// WiFi 재연결 지수 백오프 (2초 → 최대 60초)
unsigned long lastReconnectMs    = 0;
unsigned long reconnectBackoffMs = 2000;

// ── 학습 데이터 CSV 로깅 ──────────────────────────────────────────
// 'log <label>' 시 모니터링 중 매 측정마다 라벨 포함 CSV 행을 출력.
// 실험자가 물리적으로 상황을 만들고(예: 클램프 조여 느리게) 라벨을 지정 →
// 그 구간 데이터에 자동으로 라벨이 찍힘 (ground-truth, 수동 편집 불필요).
//   log normal / log slow / log fast  → 로깅 ON + 해당 라벨
//   log off                            → 로깅 OFF
// 빈 문자열이면 로깅 OFF.
char csvLabel[8] = "";

// =================================================================
// BLE IV 상태 연동 (앱 통신)
// =================================================================
// IVPhase enum 순서와 일치 (PHASE_BASELINE 포함)
const char* ivPhaseStr(IVPhase phase) {
  static const char *names[] = {
    "boot","tare","wait","stabilize","baseline","warmup","monitor","done"
  };
  return (phase >= 0 && phase <= PHASE_DONE) ? names[phase] : "unknown";
}

// IV Phase 전환/상태 요청 시 BLE 로 앱에 notify
void notifyPhaseChange() {
  bleProv.notifyIvStatus(
    ivPhaseStr(ivPhase), iv.currentWeight, iv.currentFlowRate, FW_VERSION
  );
}

// BLE 커맨드 처리 (앱 → ESP32: reset/tare/status) — loop 에서 호출
void handleBleCommands() {
  if (bleProv.command.resetPending) {
    bleProv.command.resetPending = false;
    Serial.println("[BLE CMD] reset — IV 상태 초기화");
    iv = IVState{};
    ivPhase = PHASE_TARE;
    detector.reset();
    notifyPhaseChange();
  }
  if (bleProv.command.tarePending) {
    bleProv.command.tarePending = false;
    Serial.println("[BLE CMD] tare — 수동 영점 조정");
    loadCell.tare(20);
    Serial.println("[BLE CMD] 영점 완료.");
    notifyPhaseChange();
  }
  if (bleProv.command.statusPending) {
    bleProv.command.statusPending = false;
    Serial.println("[BLE CMD] status — IV 상태 전송");
    notifyPhaseChange();
  }
}

// =================================================================
// 기기 ID 생성 — ESP32 칩 고유 MAC으로 식별
// =================================================================
void buildDeviceId() {
  // 테스트 기기 1대 — QR 코드와 동일한 이름 고정
  // QR: {"name":"INFUCARE_A1B2","pop":"infucare123"}
  snprintf(deviceId, sizeof(deviceId), "INFUCARE_A1B2");
  snprintf(bleName,  sizeof(bleName),  "%s", deviceId);
}

// ─────────────────────────────────────────────────────────────────
// HTTP REST API 전송 함수
// ─────────────────────────────────────────────────────────────────

// 측정 데이터 전송 (5초 주기) — 백엔드 MeasurementCreate 스키마에 맞춤.
// POST /api/v1/measurements
//   필수: remaining_ml, drop_rate, infusion_status  (weight_g/measured_at 선택)
//   백엔드가 weight_g + 세션 tare 로 remaining_ml 을 보정하므로, remaining_ml 엔
//   무게(g) 기반 추정값을 fallback 으로 보낸다. (1g ≈ 1mL 가정)
void httpSendMeasurement() {
  if (!httpEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/v1/measurements";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Serial", deviceId);
  http.addHeader("X-Device-Key", DEVICE_KEY);     // 백엔드 필수 헤더

  StaticJsonDocument<192> doc;
  doc["weight_g"]        = round(iv.currentWeight   * 100) / 100.0;     // 무게 (g)
  doc["remaining_ml"]    = round(iv.currentWeight   * 100) / 100.0;     // 잔량 추정 (백엔드 보정)
  doc["drop_rate"]       = round(iv.currentFlowRate * 10000) / 10000.0; // 유속 (g/s)
  doc["infusion_status"] = iv.complete ? "completed" : "running";
  char buf[192];
  serializeJson(doc, buf);

  int code = http.POST(buf);
  if (code == 200 || code == 201) Serial.println("[HTTP] 측정 전송 OK");
  else                            Serial.printf("[HTTP] 측정 전송 실패: %d\n", code);
  http.end();
}

// 이상 알림 전송 — 백엔드 AlertCreate 스키마에 맞춤.
//   POST /api/v1/alerts  body: { "alert_type": "flow_fast"|"flow_slow", "message": str }
//   백엔드 title_map: flow_fast→"유속 과다", flow_slow→"유속 저하"
//   level("주의"/"경고")은 백엔드에 별도 필드가 없으므로 message 에 표기한다.
//     주의 : ±1 이상값 WARN_CONSEC개 연속 (조기 경보)
//     경고 : CNN 확정 이상
//   dir : FLOW_SLOW(느림) / FLOW_FAST(빠름)
void httpSendAlert(const char *level, int dir) {
  const char *alertType = (dir == FLOW_FAST) ? "flow_fast" : "flow_slow";
  const char *statusKo  = (dir == FLOW_FAST) ? "빠름" : "느림";
  float conf = detector.getWindowConfidence();
  int   abn  = detector.getLastAbnCount();

  // 단계/상세를 담은 메시지 (백엔드 Notification.message 로 저장됨)
  char msg[128];
  if (strcmp(level, "경고") == 0)
    snprintf(msg, sizeof(msg), "[경고] 유속 %s 확정 (CNN 신뢰도 %.0f%%, 이상값 %d/%d)",
             statusKo, conf * 100.0f, abn, CNN_WIN);
  else
    snprintf(msg, sizeof(msg), "[주의] 유속 %s 의심 (이상값 %d개 연속)",
             statusKo, WARN_CONSEC);

  // 시리얼 출력 — httpEnabled 무관하게 항상 (데이터 수집 모드에서도 확인 가능)
  Serial.printf("[%s] ⚠️ %s\n", level, msg);

  if (!httpEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/v1/alerts";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Serial", deviceId);
  http.addHeader("X-Device-Key", DEVICE_KEY);     // 백엔드 필수 헤더

  StaticJsonDocument<192> doc;
  doc["alert_type"] = alertType;     // "flow_fast" / "flow_slow"
  doc["message"]    = msg;
  char buf[192];
  serializeJson(doc, buf);

  int code = http.POST(buf);
  if (code == 200 || code == 201) Serial.printf("[HTTP] 이상 %s 전송 OK\n", level);
  else                            Serial.printf("[HTTP] 이상 %s 실패: %d\n", level, code);
  http.end();
}

// 하트비트 전송 — 현재 비활성.
// 백엔드 실제 경로는 POST /api/v1/devices/{device_id}/heartbeat 로, 경로에 DB가
// 부여한 "정수 device_id" 가 필요하다. 펌웨어는 시리얼(device_uid)만 알고 정수
// device_id 는 모르므로 이 엔드포인트를 호출할 수 없다.
// 다행히 백엔드는 /measurements 수신 시마다 last_seen_at 을 갱신하므로(5초 주기),
// 모니터링 중 생존 신호는 측정 전송으로 대체된다. → 별도 하트비트 불필요.
// (idle 구간 생존 표시가 필요하면, 프로비저닝 시 정수 device_id 를 기기에 전달하도록
//  백엔드/앱 협의 후 이 함수를 device_id 경로로 복구할 것.)
void httpSendHeartbeat() {
  return;   // 비활성 — 위 주석 참고 (측정 전송이 last_seen_at 갱신을 대신함)
}

// ─────────────────────────────────────────────────────────────────
// CNN 이미지 시각화 (시리얼 ASCII 출력, 디버그용)
// ─────────────────────────────────────────────────────────────────
void printImage() {
  int img[CNN_DIM][CNN_DIM];
  detector.getImage(img);
  Serial.println("[CNN] ┌────┐");
  for (int r = 0; r < CNN_DIM; r++) {
    Serial.print("      │");
    for (int c = 0; c < CNN_DIM; c++) {
      if      (img[r][c] == FLOW_FAST) Serial.print('+');
      else if (img[r][c] == FLOW_SLOW) Serial.print('-');
      else                             Serial.print('.');
    }
    Serial.println("│");
  }
  Serial.println("      └────┘");
  Serial.printf("  + 빠름  · 정상  - 느림   score:%.3f\n",
                detector.getWindowConfidence());
}

// ─────────────────────────────────────────────────────────────────
// 시리얼 디버그 명령 (엔지니어용, 실무 사용 불필요)
// ─────────────────────────────────────────────────────────────────
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // ── rebaseline : 기준 유속 재측정 (디버그) ─────────────────────
  // gtt 입력 없음. 필요 시 재영점부터 다시 (baseline 자가 측정).
  if (line == "rebaseline") {
    iv = IVState{};  ivPhase = PHASE_TARE;  detector.reset();
    Serial.println("[DBG] 재영점 → 기준 유속 재측정 시작.");
  } else if (line.startsWith("finish ")) {
    iv.finishWeight = line.substring(7).toFloat();
    Serial.printf("[DBG] 종료 무게: %.2f g\n", iv.finishWeight);
  } else if (line == "tare") {
    loadCell.tare(20);
    Serial.println("[DBG] 영점 완료.");
  } else if (line == "reset") {
    iv = IVState{};  ivPhase = PHASE_TARE;  detector.reset();
    Serial.println("[DBG] 초기화 → 재영점.");
  } else if (line == "wifireset") {
    // 저장된 WiFi 자격증명 삭제 → 재부팅 후 BLE 프로비저닝 모드 진입
    Serial.println("[DBG] WiFi 자격증명 삭제 후 재부팅...");
    bleProv.clearStoredCredentials();
    WiFi.disconnect(true, true);
    delay(500);
    ESP.restart();
  } else if (line == "status") {
    const char *ph[] = { "부팅대기","영점조정","수액대기","흔들림안정화",
                         "기준유속측정","EMA안정화","모니터링","완료" };
    Serial.printf("[STATUS] 단계:%s  W:%.2fg\n"
                  "         기준유속:%.4f g/s  현재유속:%.4f g/s  종료무게:%.1fg\n"
                  "         결과:%s  신뢰도:%.0f%%  CNN:%s  샘플:%d/%d\n",
                  ph[ivPhase], iv.currentWeight,
                  iv.baselineFlowRate, iv.currentFlowRate, iv.finishWeight,
                  detector.getResultLabel(),
                  detector.getWindowConfidence() * 100.0f,
                  detector.isTFLiteActive() ? "ON" : "fallback",
                  detector.getSampleCount(), CNN_WIN);
  } else if (line.startsWith("tolerance ")) {
    float t = line.substring(10).toFloat();
    if (t > 0 && t < 1.0f) { detector.setTolerance(t); Serial.printf("[DBG] tolerance=±%.0f%%\n", t*100); }
  } else if (line.startsWith("alpha ")) {
    float a = constrain(line.substring(6).toFloat(), 0.05f, 0.5f);
    loadCell.setEmaAlpha(a);  Serial.printf("[DBG] EMA alpha=%.2f\n", a);
  } else if (line == "http on") {
    httpEnabled = true;
    Serial.println("[DBG] HTTP 전송 ON");
  } else if (line == "http off") {
    httpEnabled = false;
    Serial.println("[DBG] HTTP 전송 OFF (서버 없이 데이터 수집 시 권장)");
  } else if (line == "log off") {
    csvLabel[0] = '\0';
    Serial.println("[DBG] CSV 로깅 OFF");
  } else if (line.startsWith("log ")) {
    String lab = line.substring(4);
    lab.trim();
    if (lab == "normal" || lab == "slow" || lab == "fast") {
      snprintf(csvLabel, sizeof(csvLabel), "%s", lab.c_str());
      Serial.printf("LOG_HEADER,ms,flow_gs,target_gs,state,label\n");
      Serial.printf("[DBG] CSV 로깅 ON — 라벨:'%s'. 'LOG,' 줄을 캡처하세요.\n", csvLabel);
      Serial.println("[DBG]   상황 바뀌면 log normal/slow/fast 로 라벨 변경, log off 로 종료.");
    } else {
      Serial.println("[DBG] 사용법: log normal | log slow | log fast | log off");
    }
  } else if (line == "cnn") {
    detector.printDebugInfo();
  } else if (line == "cnntest") {
    detector.runSelfTest();
  } else if (line == "cnnverbose" || line == "cnnv") {
    detector.setVerbose(!detector.getVerbose());
    Serial.printf("[DBG] CNN verbose %s\n",
                  detector.getVerbose() ? "ON (매 추론 상세 출력)" : "OFF");
  } else if (line == "image")   { printImage(); }
  else if (line == "help" || line == "?") {
    Serial.println("──── 엔지니어 시리얼 명령어 ────");
    Serial.println("  rebaseline    재영점 + 기준 유속 재측정");
    Serial.println("  finish <g>    주입 종료 무게 설정");
    Serial.println("  tare          영점 조정");
    Serial.println("  reset         전체 초기화 + 재영점");
    Serial.println("  wifireset     WiFi 자격증명 삭제 + 재부팅");
    Serial.println("  status        현재 상태 출력");
    Serial.println("  tolerance <t> 이상감지 허용 오차 (0~1)");
    Serial.println("  alpha <a>     EMA 계수 (0.05~0.5)");
    Serial.println("  http on/off   HTTP 서버 전송 토글 (서버 없을 땐 off 로 로그 깔끔)");
    Serial.println("  log <label>   학습 데이터 CSV 로깅 (normal/slow/fast), log off 종료");
    Serial.println("  cnn           CNN 엔진 진단 (모드/확률/텐서 정보)");
    Serial.println("  cnntest       알려진 패턴으로 추론 검증 (TFLite 증명)");
    Serial.println("  cnnv          CNN 추론 상세 로그 토글");
    Serial.println("  image         CNN 이미지 출력");
  }
  else {
    Serial.printf("[DBG] 모르는 명령: %s   ('help' 입력)\n", line.c_str());
  }
}

// ─────────────────────────────────────────────────────────────────
// setup
// ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart IV Pole v1.3 ===");

  // ── 워치독 초기화 ────────────────────────────────────────────────
  // BLE/WiFi/HTTP 블로킹으로 멈추면 자동 리셋. baseline 측정 중에도
  // loop 가 계속 돌며 esp_task_wdt_reset() 호출되므로 안전.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  const esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdtCfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
  Serial.printf("[WDT] 워치독 활성화 (%ds)\n", WDT_TIMEOUT_SEC);

  // ── 기기 ID 생성 ────────────────────────────────────────────────
  buildDeviceId();

  // ── 부팅 시 기기 정보 출력 (QR 라벨 제작용) ─────────────────────
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.printf( "║  기기 ID : %-34s║\n", deviceId);
  Serial.printf( "║  BLE 이름: %-34s║\n", bleName);
  Serial.printf( "║  FW 버전 : %-34s║\n", FW_VERSION);
  Serial.printf( "║  서버 URL: %-34s║\n", SERVER_URL);
  Serial.println("╠══════════════════════════════════════════════╣");
  Serial.printf( "║  QR JSON : {\"name\":\"%s\",\"pop\":\"infucare123\"}  ║\n", deviceId);
  Serial.println("╚══════════════════════════════════════════════╝");

  // ── 로드셀 초기화 ────────────────────────────────────────────────
  loadCell.begin(128);
  loadCell.setCalibFactor(CALIB_FACTOR_DEFAULT);
  Serial.printf("[ADS] CalibFactor=%.2f\n", loadCell.getCalibFactor());

  // ── WiFi 연결 (커스텀 BLE 프로비저닝) — 전부 논블로킹 ────────────
  //   1) 저장된 자격증명으로 WiFi.begin() 만 호출 (결과 대기 안 함)
  //   2) BLE GATT 서버 즉시 시작 (WiFi 연결과 병렬)
  //   3) WiFi 연결은 loop() 에서 비동기 체크 (지수 백오프 재연결)
  //   4) WiFi 미연결 상태에서도 메인 루프 진입 (HTTP 는 WiFi 가드)
  bleProv.startStoredCredentials();   // 논블로킹: WiFi.begin() 만

  Serial.printf("[WiFi] BLE 프로비저닝 광고 시작. 기기명: %s\n", bleName);
  Serial.println("[WiFi] 앱에서 언제든 WiFi 재설정 가능합니다.");
  bleProv.begin(bleName);

  if (WiFi.status() != WL_CONNECTED)
    Serial.println("[WiFi] 연결 진행 중 — 메인 루프 시작 (백그라운드 연결).");
  else
    Serial.printf("[WiFi] 연결 완료. IP: %s\n", WiFi.localIP().toString().c_str());

  // ── CNN 탐지기 초기화 ────────────────────────────────────────────
  if (detector.begin())
    Serial.println("[CNN] ✓ TFLite 학습 모델 활성화됨.");
  else
    Serial.println("[CNN] ⚠️ Fallback 모드 (학습 모델 미탑재). 'cnn' 명령으로 진단.");

  // ── HTTP REST API 모드 ──────────────────────────────────────────
  Serial.printf("[HTTP] 서버: %s\n", SERVER_URL);
  Serial.printf("[HTTP] 측정 전송 %dms / 하트비트 %dms\n", HTTP_SEND_MS, HEARTBEAT_MS);

  // ── 부팅 타이머 시작 → 2초 후 자동 영점 ──────────────────────────
  bootMs = millis();
  Serial.printf("[SYS] %.1f초 후 자동 영점 조정 시작.\n", BOOT_DELAY_MS / 1000.0f);
  Serial.println("[SYS] 수액 걸면 기준 유속을 자동 측정합니다 (gtt 입력 불필요).");

  lastWeightMs = millis();
}

// ─────────────────────────────────────────────────────────────────
// loop
// ─────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  esp_task_wdt_reset();    // 워치독 리셋 (매 루프)

  handleSerial();
  bleProv.loop();          // BLE 백그라운드 처리 (논블로킹 WiFi 포함)
  handleBleCommands();     // 앱 커맨드 처리 (reset/tare/status)

  // ── WiFi 재연결 감시 (지수 백오프) ──────────────────────────────
  if (WiFi.status() != WL_CONNECTED && !bleProv.isConnecting()) {
    if (now - lastReconnectMs >= reconnectBackoffMs) {
      lastReconnectMs = now;
      WiFi.reconnect();
      // 백오프: 2s → 4s → ... → 60s (최대)
      reconnectBackoffMs = min(reconnectBackoffMs * 2, 60000UL);
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    reconnectBackoffMs = 2000;   // 연결 성공 시 백오프 리셋
  }

  // HTTP 하트비트 (60초 주기)
  if (WiFi.status() == WL_CONNECTED && now - lastHeartbeatMs >= HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    httpSendHeartbeat();
  }

  // =================================================================
  // 자동 부팅 절차 상태 머신
  // =================================================================

  // ── 1단계: 부팅 대기 (2초) ────────────────────────────────────────
  if (ivPhase == PHASE_BOOT) {
    if (now - bootMs >= BOOT_DELAY_MS) ivPhase = PHASE_TARE;
    return;
  }

  // ── 2단계: 자동 영점 조정 ─────────────────────────────────────────
  if (ivPhase == PHASE_TARE) {
    Serial.println("[AUTO] 영점 조정 중...");
    loadCell.tare(20);
    iv = IVState{};
    detector.reset();
    ivPhase = PHASE_WAIT;
    Serial.println("[AUTO] 영점 완료. 수액팩을 걸어주세요.");
    lastWeightMs = millis();
    return;
  }

  // ── 3단계: 수액 감지 대기 ─────────────────────────────────────────
  if (ivPhase == PHASE_WAIT && now - lastWeightMs >= WEIGHT_MS) {
    lastWeightMs = now;
    float w = loadCell.stableRead(10);   // EMA 미사용 통일
    iv.currentWeight = w;

    if (w > WEIGHT_HANG_G) {
      Serial.printf("[AUTO] 수액 감지 (%.1fg) — 흔들림 안정화 대기 시작\n", w);
      // 안정화 버퍼 초기화
      for (int i = 0; i < STABILIZE_WINDOW; i++) stabilizeBuf[i] = 0;
      stabilizeIdx     = 0;
      stabilizeCount   = 0;
      stabilizeStartMs = millis();
      ivPhase          = PHASE_STABILIZE;
    }
    return;
  }

  // ── 3-1단계: 흔들림 안정화 대기 ──────────────────────────────────
  // ★ 판정 기준 = "무게 변화율(연속 차분)의 일관성".
  //   정상 주입 중에도 무게는 계속 줄어들어 절대 변동(max-min)은 안 줄어든다
  //   (유속×윈도우시간 만큼). 따라서 절대 변동으로는 영영 안정 판정이 안 됨.
  //   흔들림 = 차분이 부호 바뀌고 들쭉날쭉(+4,-4...), 정상 감소 = 차분 일정(-0.3...).
  //   → 연속 차분의 변동폭(dmax-dmin) < 임계값이면 "안정"으로 판정.
  //   stableRead 사용 (EMA 지연 없음).
  if (ivPhase == PHASE_STABILIZE && now - lastWeightMs >= WEIGHT_MS) {
    lastWeightMs = now;
    float w = loadCell.stableRead(10);   // EMA 없는 원시 평균 (지연 없음)
    iv.currentWeight = w;

    // 수액팩이 빠졌으면 다시 대기
    if (w < WEIGHT_REMOVE_G) {
      Serial.println("[AUTO] 안정화 중 수액 제거 감지 — 다시 대기.");
      ivPhase = PHASE_WAIT;
      return;
    }

    // ring buffer 에 저장
    stabilizeBuf[stabilizeIdx] = w;
    stabilizeIdx = (stabilizeIdx + 1) % STABILIZE_WINDOW;
    if (stabilizeCount < STABILIZE_WINDOW) stabilizeCount++;

    // 윈도우 가득 차야 판정
    bool forceContinue = (now - stabilizeStartMs >= STABILIZE_TIMEOUT_MS);
    if (stabilizeCount >= STABILIZE_WINDOW) {
      // 순환버퍼 → 시간순 복원 (stabilizeIdx = 가장 오래된 자리)
      float seq[STABILIZE_WINDOW];
      for (int k = 0; k < STABILIZE_WINDOW; k++)
        seq[k] = stabilizeBuf[(stabilizeIdx + k) % STABILIZE_WINDOW];

      // 연속 차분의 변동폭 계산 (흔들림이면 큼, 일정 감소면 작음)
      float dmin = 1e9f, dmax = -1e9f;
      for (int k = 1; k < STABILIZE_WINDOW; k++) {
        float d = seq[k] - seq[k - 1];
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
      }
      float range = dmax - dmin;   // 차분 변동폭
      Serial.printf("[AUTO] 안정화 중... W:%.2fg  차분변동:%.2fg (기준 <%.2fg)\n",
                    w, range, STABILIZE_RANGE_G);

      if (range < STABILIZE_RANGE_G || forceContinue) {
        if (forceContinue)
          Serial.println("[AUTO] ⚠️ 안정화 시간 초과 — 강제로 기준 유속 측정 진입");
        else
          Serial.println("[AUTO] ✓ 안정화 완료 — 기준 유속 측정 시작");

        Serial.printf("[AUTO] 기준 유속 측정 중 (%lu초)... 평소 흐름 그대로 두세요.\n",
                      BASELINE_DURATION_MS / 1000UL);
        baselineWeightStart = loadCell.stableRead(10);
        baselineStartMs     = millis();
        ivPhase             = PHASE_BASELINE;
      }
    } else {
      Serial.printf("[AUTO] 안정화 샘플 수집 중... W:%.2fg (%d/%d)\n",
                    w, stabilizeCount, STABILIZE_WINDOW);
    }
    return;
  }

  // ── 4단계: 기준 유속(baseline) 측정 (60초) ────────────────────────
  // gtt/drip factor 없이, 측정 구간의 평균 유속을 정상 기준선으로 확정.
  if (ivPhase == PHASE_BASELINE && now - baselineStartMs >= BASELINE_DURATION_MS) {
    float weightEnd  = loadCell.stableRead(10);
    float weightLost = baselineWeightStart - weightEnd;
    float elapsedSec = (now - baselineStartMs) / 1000.0f;
    float flow       = (elapsedSec > 0) ? (weightLost / elapsedSec) : 0;

    Serial.printf("[AUTO] 측정 결과: %.3fg 감소 / %.0f초 = %.4f g/s\n",
                  weightLost, elapsedSec, flow);

    // ── sanity check: 비현실적 baseline 거부 → 재측정 ──────────────
    if (flow < BASELINE_MIN_FLOW) {
      Serial.printf("[AUTO] ⚠️ 유속 너무 느림(%.4f < %.4f) — 막힘 의심. 재측정.\n",
                    flow, BASELINE_MIN_FLOW);
      baselineWeightStart = loadCell.stableRead(10);
      baselineStartMs     = millis();   // 다시 측정
      return;
    }
    if (flow > BASELINE_MAX_FLOW) {
      Serial.printf("[AUTO] ⚠️ 유속 비현실적(%.4f > %.4f) — 재측정.\n",
                    flow, BASELINE_MAX_FLOW);
      baselineWeightStart = loadCell.stableRead(10);
      baselineStartMs     = millis();
      return;
    }

    // ── baseline 확정 ─────────────────────────────────────────────
    iv.baselineFlowRate = flow;
    Serial.printf("[AUTO] ✓ 기준 유속 확정: %.4f g/s (이 속도가 정상 기준)\n",
                  iv.baselineFlowRate);

    // ※ 모니터링도 stableRead 사용 (EMA 미사용) → 첫 prevWeight 만 세팅.
    iv.currentWeight = loadCell.stableRead(10);
    iv.prevWeight    = iv.currentWeight;
    iv.started       = true;
    iv.complete      = false;
    iv.warmup        = WARMUP_SAMPLES;
    ivPhase          = PHASE_WARMUP;
    Serial.printf("[AUTO] 측정 준비 중... (%d 샘플)\n", WARMUP_SAMPLES);
    lastWeightMs = millis();
    return;
  }

  // =================================================================
  // 무게 측정 주기 — WAIT/STABILIZE/BASELINE 단계는 위에서 따로 처리
  // ★ stableRead 사용 (EMA 미사용): EMA 지연이 전이(빠름↔느림)를 왜곡해
  //   학습/판정을 오염시키므로, 순간 무게를 즉시 반영. 노이즈는 CNN 이 흡수.
  // =================================================================
  if (ivPhase != PHASE_WAIT && ivPhase != PHASE_STABILIZE
      && ivPhase != PHASE_BASELINE
      && now - lastWeightMs >= WEIGHT_MS) {
    lastWeightMs = now;

    iv.prevWeight    = iv.currentWeight;
    iv.currentWeight = loadCell.stableRead(10);

    // ── 측정 준비 단계 (첫 prevWeight 세팅) ───────────────────────
    if (ivPhase == PHASE_WARMUP) {
      iv.prevWeight = iv.currentWeight;   // 유속 계산 없이 prev 만 갱신
      iv.warmup--;
      Serial.printf("[AUTO] 측정 준비 중... W:%.2fg  (남은 샘플 %d개)\n",
                    iv.currentWeight, iv.warmup);
      if (iv.warmup == 0) {
        ivPhase = PHASE_MONITOR;
        Serial.println("[AUTO] ✓ 모니터링 시작!");
      }
      return;
    }

    // ── 모니터링 단계 ──────────────────────────────────────────────
    if (ivPhase == PHASE_MONITOR) {

      // 수액팩 제거 감지 → 자동 재영점
      if (iv.currentWeight < WEIGHT_REMOVE_G) {
        Serial.println("[AUTO] 수액 제거 감지 — 재영점 후 재시작.");
        iv      = IVState{};
        ivPhase = PHASE_TARE;
        detector.reset();
        return;
      }

      // 유속 계산: g/s = 무게 감소량 ÷ 측정 주기
      iv.currentFlowRate = (iv.prevWeight - iv.currentWeight)
                           / (WEIGHT_MS / 1000.0f);

      // CNN 샘플 추가 (16개 채워지면 자동 분류)
      detector.addSample(iv.currentFlowRate, iv.baselineFlowRate);

      // ── 학습 데이터 CSV 로깅 (라벨 포함) ──────────────────────────
      // 형식: LOG,<ms>,<flow_gs>,<target_gs>,<state>,<label>
      //   state: -1=느림 0=정상 1=빠름 (detector 단일 샘플 분류값)
      //   label: log <label> 로 실험자가 지정한 ground-truth
      if (csvLabel[0] != '\0') {
        Serial.printf("LOG,%lu,%.4f,%.4f,%d,%s\n",
                      now, iv.currentFlowRate, iv.baselineFlowRate,
                      detector.getLastState(), csvLabel);
      }

      // 2단계 알람
      //   주의: ±1 이 WARN_CONSEC개 연속 → 즉시 발동 (윈도우 완성 무관)
      //   경고: 16샘플 윈도우 완성 후 CNN 확정 이상 (상위 — 같은 틱이면 주의 흡수)
      if (detector.detectAnomaly()) {
        detector.detectCaution();                         // 같은 틱 주의 흡수(경고 우선)
        httpSendAlert("경고", detector.getWindowResult());
      } else if (detector.detectCaution()) {
        httpSendAlert("주의", detector.getCautionDir());
      }

      // 주입 완료 판정 (종료 무게 설정 시)
      if (iv.finishWeight > 0 && iv.currentWeight <= iv.finishWeight) {
        iv.complete = true;
        iv.started  = false;
        ivPhase     = PHASE_DONE;
        Serial.println("[IV] 주입 완료.");
        return;
      }

      // 측정 결과 출력 — 부호(s) 표기: '-'(느림) '0'(정상) '+'(빠름)
      int  s  = detector.getLastState();
      char sc = (s < 0) ? '-' : (s > 0) ? '+' : '0';
      if (detector.getSampleCount() == 0 && detector.isWindowFull()) {
        Serial.printf("[IV] W:%.2fg  유속:%.4f/%.4fg/s  부호:%c  → %s (신뢰도:%.0f%%, ±1 %d/%d)\n",
                      iv.currentWeight, iv.currentFlowRate, iv.baselineFlowRate, sc,
                      detector.getResultLabel(),
                      detector.getWindowConfidence() * 100.0f,
                      detector.getLastAbnCount(), CNN_WIN);
      } else {
        Serial.printf("[IV] W:%.2fg  유속:%.4f/%.4fg/s  부호:%c  [수집중 %d/%d, 연속%d, ±1 %d]\n",
                      iv.currentWeight, iv.currentFlowRate, iv.baselineFlowRate, sc,
                      detector.getSampleCount(), CNN_WIN,
                      detector.getConsecAbn(), detector.getCurrentAbnCount());
      }
    }

    // ── 주입 완료 후 수액 제거 감지 ───────────────────────────────
    if (ivPhase == PHASE_DONE && iv.currentWeight < WEIGHT_REMOVE_G) {
      Serial.println("[AUTO] 빈 수액 제거 — 재영점 후 대기 중.");
      iv      = IVState{};
      ivPhase = PHASE_TARE;
      detector.reset();
    }
  }

  // ── HTTP 측정 전송 (5초 주기, 모니터링 중에만) ───────────────────
  // 유속 + 무게만. 상태는 이상 감지 시 httpSendAlert() 로 별도 전송.
  if (WiFi.status() == WL_CONNECTED && ivPhase == PHASE_MONITOR
      && now - lastHttpSendMs >= HTTP_SEND_MS) {
    lastHttpSendMs = now;
    httpSendMeasurement();
  }

  // ── IV phase 변화 시 BLE 로 앱에 자동 notify ─────────────────────
  static IVPhase lastNotifiedPhase = PHASE_BOOT;
  if (ivPhase != lastNotifiedPhase) {
    lastNotifiedPhase = ivPhase;
    notifyPhaseChange();
  }
}
