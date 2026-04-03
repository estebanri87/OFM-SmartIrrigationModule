#pragma once

#include <cstdint>
#include <cstdlib>
#include <algorithm>

namespace SmartIrrigation
{
    // M-4: Common utility - moved from anonymous namespaces in .cpp files
    inline float clampFloat(float value, float minValue, float maxValue)
    {
        if (value < minValue) return minValue;
        if (value > maxValue) return maxValue;
        return value;
    }
    struct RealWeatherInputs
    {
        bool hasTemperature = false;
        float temperatureC = 0.0f;
        bool hasRain = false;
        bool rainActive = false;
        bool hasRainAmount = false;
        float rainAmountMm = 0.0f;
        // Wind speed kept for wind-pause safety feature
        bool hasWind = false;
        float windSpeedMs = 0.0f;
    };

    struct ForecastWeatherInputs
    {
        // ET0 per day, day 0 = today, day 6 = 6 days ahead
        // Provided via KOs from InternetWeatherModule or any KNX source
        static constexpr uint8_t kForecastDays = 7;
        bool hasEt0Day[kForecastDays] = {};
        float et0DayMm[kForecastDays] = {};
        // Rain amounts per day (mm/day), day 0 = today
        bool hasRainDay[kForecastDays] = {};
        float rainDayMm[kForecastDays] = {};
    };

    struct WeatherInputs
    {
        RealWeatherInputs real{};
        ForecastWeatherInputs forecast{};
    };

    struct TimeWindow
    {
        bool active = false;
        uint16_t startMinutes = 0;
        uint16_t endMinutes = 0;
    };

    enum class TimeWindowType : uint8_t
    {
        Flexible = 0,
        Fixed = 1,
        SunriseBased = 2,
        SunsetBased = 3
    };

    // N-1: Single-window activity check — shared by Core and Module logic
    inline bool isWindowActive(uint16_t nowMinutes, const TimeWindow &window)
    {
        if (!window.active) return false;
        if (window.endMinutes >= window.startMinutes)
            return nowMinutes >= window.startMinutes && nowMinutes <= window.endMinutes;
        return nowMinutes >= window.startMinutes || nowMinutes <= window.endMinutes;
    }

    // N-2: Fixed-window exact-match (±1 min, midnight-wrap-aware)
    inline bool isFixedWindowMatch(uint16_t nowMinutes, uint16_t startMinutes)
    {
        const int diff = std::abs(static_cast<int>(nowMinutes) - static_cast<int>(startMinutes));
        const int circularDiff = std::min(diff, 1440 - diff);
        return circularDiff <= 1;
    }

    enum class FlowInputUnit : uint8_t
    {
        MmPerHour = 0,
        LitersPerMinute = 1,
        CubicMetersPerHour = 2
    };

    static constexpr uint8_t MAX_SPRINKLERS = 6;

    struct SprinklerConfig
    {
        FlowInputUnit unit = FlowInputUnit::MmPerHour;
        uint8_t count = 1;
        float values[MAX_SPRINKLERS] = {0.0f};

        // Calculate effective precipitation rate in mm/h for the zone
        // For mm/h: average of all sprinkler values
        // For l/min or m³/h: sum flow, convert to l/h, divide by area → mm/h
        float calcPrecipRateMmh(float areaM2) const;

        // Calculate total flow in l/min (for statistics/tank)
        float calcTotalFlowLpm() const;
    };

    struct TimeWindowCheck
    {
        bool allowed = false;
        bool conflict = false;
        bool fixedMatch = false;
    };

    struct ZoneSettings
    {
        bool enabled = false;
        float areaM2 = 0.0f;
        // Sprinkler config (Smart Pro) or simple precip rate (Smart Core)
        bool isSmartPro = false;
        bool isDrip = false;
        float precipRateMmh = 0.0f;       // Smart Core: direct mm/h input; or computed result
        SprinklerConfig sprinklerConfig{}; // Smart Pro: detailed sprinkler setup
        float dripFlowLph = 0.0f;          // Drip: total flow in l/h
        uint8_t etFactorPercent = 100;
        uint8_t interceptionPercent = 0;
        uint8_t minWeek = 0;
        uint8_t maxWeek = 0;
        uint8_t absMaxWeek = 0;
        uint8_t maxCycles = 0;
        uint8_t minPerCycle = 0;
        uint16_t manualRuntimeMinutes = 0;
        uint8_t baseTempC = 0;
        uint8_t baseTempHyst = 0;
        uint8_t soilThresholdPercent = 0;
        bool soilMoistureEnabled = false;
        float rainDelayFactor = 0.0f;
        uint16_t maxRuntimeMinutes = 0;
        TimeWindow window1{};
        TimeWindow window2{};
        TimeWindowType windowType = TimeWindowType::Flexible;
        // Phase 1: Check-Back
        bool checkBackEnabled = false;
        uint8_t checkBackDelaySec = 30;
        // Phase 2: Sunrise/Sunset (2.1)
        int8_t sunOffsetMinutes = 0;
        uint16_t sunWindowDurationMinutes = 240;
        // Phase 2: Soak-Time (2.2)
        uint8_t soakTimeMinutes = 0;
        // Phase 2: Rest Days (2.3)
        uint8_t irrigationIntervalDays = 0;  // 0 = 7 days (default), otherwise custom interval
        // Phase 3: Activity/Wind (3.2, 3.5)
        bool isSprinkler = true;
        // Phase 3: Weekday Filter (3.3)
        uint8_t allowedWeekdays = 0x7F;  // Bit 0=Mo..6=So, default all days
        // Phase 4.3: Post-Irrigation Verify
        bool verifyEnabled = false;
        uint8_t verifyDelayMinutes = 30;
        uint8_t verifyMinDeltaPercent = 5;
        // Bucket model & Lead Time
        uint8_t soilType = 0;              // 0=Deaktiviert, 1-5=Presets, 6=Benutzerdefiniert
        uint16_t maxBucketMm = 0;          // Maximum soil water capacity in mm (0=disabled)
        uint16_t drainageRateRaw = 0;      // Raw value ×0.1 = mm/h (0=disabled)
        uint16_t leadTimeSeconds = 0;      // Extra lead time added to runtime (seconds)
        uint8_t zonePriority = 5;          // User-configured priority 0-10 (default 5)
        bool activityEnabled = false;      // Per-zone activity block enabled
    };

    struct ZoneRuntimeState
    {
        float weekAmount = 0.0f;
        uint8_t cycles = 0;
    };

    struct DecisionContext
    {
        bool systemOn = false;
        bool emergencyStop = false;
        bool rainLockActive = false;
        uint16_t nowMinutes = 0;
        bool allowWhenNoWindow = true;
        int8_t minTempC = 0;
        // Per-zone soil moisture for override (critically dry soil bypasses rain-lock)
        bool hasSoilMoisture = false;
        float soilMoisturePercent = 0.0f;
    };

    enum class DecisionMode : uint8_t
    {
        Real = 1,
        Forecast = 2,
        Mixed = 3,
        Fallback = 4
    };

    enum class DecisionAction : uint8_t
    {
        None = 0,
        WaitTimeWindow = 1,
        Start = 2
    };

    struct DecisionResult
    {
        DecisionAction action = DecisionAction::None;
        DecisionMode mode = DecisionMode::Fallback;
        float waterDemand = 0.0f;
        float et0 = 0.0f;
        float effectiveRain = 0.0f;
        bool soilOverride = false;
        bool fallbackActive = false;
        uint8_t errorCode = 0;
        bool timeWindowConflict = false;
    };

    enum class SensorId : uint8_t
    {
        Temperature = 0,
        Rain,
        RainAmount,
        Wind, // kept for wind-pause safety feature
        Count
    };

    class SensorTimeoutTracker
    {
      public:
                SensorTimeoutTracker(uint8_t maxInvalid = 3);
        bool update(bool valid);
        void reset();
        bool failed() const;
        uint8_t invalidCount() const;

      private:
        uint8_t maxInvalid_ = 3;
        uint8_t invalidCount_ = 0;
        bool failed_ = false;
    };

    float calculateRainLockHours(float rainAmountMm, float factor, float remainingHours);
    float calculateRuntimeMinutes(float precipRateMmh, float waterDemandMm, uint16_t maxRuntimeMinutes);

    // Resolve effective precipitation rate for a zone (handles all modes)
    float getEffectivePrecipRate(const ZoneSettings &zone);

    // Bucket model: resolve maxBucket from soilType presets or custom value
    uint16_t resolveMaxBucketMm(const ZoneSettings &zone);
    // Bucket model: cap water demand at max bucket capacity
    float applyMaxBucket(float waterDemandMm, uint16_t maxBucketMm);
    // Bucket model: drainage rate in mm/h (raw ×0.1)
    float drainageRateMmh(uint16_t drainageRateRaw);

    DecisionResult decideWatering(const ZoneSettings &zone,
                                 const ZoneRuntimeState &runtime,
                                 const DecisionContext &context,
                                 const WeatherInputs &weather);

    TimeWindowCheck inTimeWindow(uint16_t nowMinutes,
                                 const TimeWindow &window1,
                                 const TimeWindow &window2,
                                 TimeWindowType type,
                                 bool allowWhenInactive = true);
}
