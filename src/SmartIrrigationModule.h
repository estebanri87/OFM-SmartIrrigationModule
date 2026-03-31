#pragma once
#include <array>
#include <cstdint>

#include "OpenKNX.h"
#include "SmartIrrigationCore.h"

class SmartIrrigationModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;

    void init() override;
    void setup(bool configured) override;
    void loop(bool configured) override;

    uint16_t flashSize() override;
    void writeFlash() override;
    void readFlash(const uint8_t *data, const uint16_t size) override;

  #if (MASK_VERSION & 0x0900) != 0x0900 // Coupler do not have GroupObjects
    void processInputKo(GroupObject &ko) override;
  #endif

  private:
    struct ZoneRuntime
    {
        float weekAmount = 0.0f;
        float weekAmountPrev = 0.0f;
        uint8_t cycles = 0;
      uint8_t state = 0;
      uint8_t errorCode = 0;
      bool errorActive = false;
      uint32_t startTimeSec = 0;
      uint32_t plannedRuntimeSec = 0;
      uint16_t remainingMinutes = 0;
      uint32_t lastTimeSec = 0;
      float lastAmount = 0.0f;
      uint32_t nextTimeSec = 0;
      float nextAmount = 0.0f;
      bool manualRun = false;
      uint16_t pauseRemainingMinutes = 0;
      uint32_t pauseStartSec = 0;
      uint32_t soakEndSec = 0;
      uint32_t checkBackTimer = 0;
      uint8_t checkBackRetries = 0;
      bool valveFeedbackReceived = false;
      // Phase 2: Rest Days (2.3)
      uint16_t lastIrrigationDayOfYear = 0;
      // Phase 3: Activity/Wind pause flags (3.2, 3.5)
      bool activityPaused = false;
      bool windPaused = false;
      // Phase 4.3: Post-Irrigation Verify
      uint32_t verifyTimerMs = 0;
      uint8_t verifyStartSoil = 0;  // Soil moisture % before irrigation
    };

    struct RainLockState
    {
      float remainingHours = 0.0f;
      uint32_t lastUpdateMs = 0;
      bool active = false;
      uint16_t lastSentHours = 0;
      bool lastSentActive = false;
    };

    static constexpr uint8_t kMaxZones = 10;
    static constexpr uint8_t kFlashVersion = 3;

    std::array<ZoneRuntime, kMaxZones> zoneRuntime_{};
    SmartIrrigation::ForecastWeights forecastWeights_{};
    SmartIrrigation::MixWeights mixWeights_{};
    std::array<SmartIrrigation::SensorTimeoutTracker,
               static_cast<size_t>(SmartIrrigation::SensorId::Count)>
        sensorHealth_{};
    std::array<uint32_t, static_cast<size_t>(SmartIrrigation::SensorId::Count)> sensorLastValidMs_{};
    SmartIrrigation::WeatherInputs weatherCache_{};
    RainLockState rainLock_{};
    uint32_t weatherUpdateLastSec_ = 0;
    uint32_t lastDecisionMs_ = 0;
    uint16_t lastResetYear_ = 0;
    uint8_t lastResetMonth_ = 0;
    uint8_t lastResetDay_ = 0;
    // 1.1 Check-Back
    // (per-zone state in ZoneRuntime)
    // 1.2 Flow Sensor
    float currentFlowLpm_ = 0.0f;
    uint32_t flowLastUpdateMs_ = 0;
    uint32_t flowAlarmStartMs_ = 0;
    bool flowAlarmActive_ = false;
    // 1.3 Master Valve
    bool masterValveOn_ = false;
    uint32_t masterPostambleEndSec_ = 0;
    uint32_t preambleEndSec_ = 0;
    // 1.4 Post-Freeze Delay
    uint32_t freezeEndSec_ = 0;
    bool wasMinTempBlocked_ = false;
    // 1.5 Total Week Amount
    float totalWeekAmount_ = 0.0f;
    float totalWeekAmountPrev_ = 0.0f;
    // 2.5 Adjustment Factor
    uint8_t adjustmentFactorPercent_ = 100;
    // 3.1 Tank
    uint8_t tankLevelPercent_ = 100;
    bool tankAlarmActive_ = false;
    bool tankLowPaused_ = false;  // for hysteresis
    // 3.2 Activity Block
    bool presenceDetected_ = false;
    bool doorOpen_ = false;
    uint32_t activityChangeMs_ = 0;
    bool activityBlocking_ = false;
    // 3.4 Season
    bool seasonEnabled_ = false;
    uint8_t seasonStartMonth_ = 3;
    uint8_t seasonEndMonth_ = 10;
    // 3.5 Wind Pause
    uint8_t windPauseThresholdKmh_ = 0;
    bool windPauseActive_ = false;
    // 4.1 Inter-Zone Delay
    uint32_t lastZoneStopMs_ = 0;
    uint8_t zoneSwitchDelaySec_ = 0;
    // 4.2 Suspend
    uint32_t suspendEndTime_ = 0;  // Epoch seconds, 0 = not suspended
    bool suspendActive_ = false;
    // AD-6: NTP robustness for Suspend
    uint32_t ntpInvalidSinceMs_ = 0;  // millis() when NTP became invalid, 0 = NTP valid

    void loadWeightsFromParams();
    void saveRuntimeToFlash(bool force = false);

    SmartIrrigation::ZoneSettings loadZoneSettings(uint8_t zoneIndex);
    void updateZoneOutputs(uint8_t zoneIndex,
           const SmartIrrigation::DecisionResult &result,
           const SmartIrrigation::ZoneSettings &settings,
           const ZoneRuntime &runtime,
           bool timeValid);
    void updateDiagOutputs(const SmartIrrigation::DecisionResult &result,
           uint8_t decisionZone);
    bool startZone(uint8_t zoneIndex,
             const SmartIrrigation::ZoneSettings &settings,
             float waterDemand,
             uint16_t manualRuntimeMinutes,
             const char *source,
             uint32_t nowSec,
             bool timeValid);
    bool stopZone(uint8_t zoneIndex,
            const SmartIrrigation::ZoneSettings &settings,
            uint32_t nowSec,
            bool timeValid);
    uint8_t stopAllZones(uint8_t zoneCount, uint32_t nowSec, bool timeValid);
    void updateActiveZoneCountdown(uint8_t zoneCount, uint32_t nowSec, bool timeValid);
    void applyWeeklyReset(uint8_t zoneCount,
      uint16_t nowMinutes,
      uint8_t dayOfWeek,
      uint16_t year,
      uint8_t month,
      uint8_t day,
      bool timeValid);
    void updateSensorStatusKo(bool sensorOk);
    void updateRainLockFromEvent(float rainAmountMm, float maxFactor);
    void updateRainLockCountdown();
    bool shouldProcessDecision(uint32_t nowMs) const;
    float maxRainDelayFactor();
};

extern SmartIrrigationModule openknxSmartIrrigationModule;
