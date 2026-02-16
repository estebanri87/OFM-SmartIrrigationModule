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
    static constexpr uint8_t kFlashVersion = 1;

    std::array<ZoneRuntime, kMaxZones> zoneRuntime_{};
    SmartIrrigation::ForecastWeights forecastWeights_{};
    SmartIrrigation::MixWeights mixWeights_{};
    std::array<SmartIrrigation::SensorTimeoutTracker,
               static_cast<size_t>(SmartIrrigation::SensorId::Count)>
        sensorHealth_{};
    SmartIrrigation::WeatherInputs weatherCache_{};
    RainLockState rainLock_{};
    uint32_t lastDecisionMs_ = 0;
    uint16_t lastResetYear_ = 0;
    uint8_t lastResetMonth_ = 0;
    uint8_t lastResetDay_ = 0;

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
    uint8_t stopAllZones(uint32_t nowSec, bool timeValid);
    void updateActiveZoneCountdown(uint32_t nowSec, bool timeValid);
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
